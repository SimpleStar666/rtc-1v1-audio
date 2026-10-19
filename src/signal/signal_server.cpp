// WebSocket 信令服务器
//
// 职责（最小可用集）：
//   1. 维护房间 -> 成员映射，两人入房即配对，满员拒入
//   2. 转发三类消息：offer / answer / candidate
//   3. 断线时清理成员并通知对端
//
// 设计说明（本项目关键决策）：
//   信令服务器【不解析媒体】，只做消息路由。与 WebRTC 的设计一致——
//   信令与媒体物理分离：信令走 TCP（可靠有序），媒体走 SRTP/UDP（低延迟可丢）。
//   服务器带宽压力因此极小（只转发几百字节的 SDP/候选）。
//
// 用法：./signal_server --port 8888
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <libwebsockets.h>

#include "common/logging.h"
#include "protocol/signaling_msg.h"

using namespace rtc;

namespace {

constexpr const char* kTag = "signal_srv";
constexpr size_t kMaxMsgBytes = 64 * 1024;
constexpr int    kMaxPeersPerRoom = 2;   // 1v1 通话：满 2 人拒入

struct Session {
    int                  peer_id = 0;
    std::string          room;
    bool                 in_room = false;
    std::vector<uint8_t> rx;     // 接收重组（LWS 可能分片投递）
    // 待发送队列。
    // 早期版本是"单槽"（std::vector + assign），导致同一处理里连发两条时
    // 后一条覆盖前一条（例如给入房者发 joined 之后紧接着发 peer_ready，
    // joined 就丢了）。改为队列后不再丢失。见 docs/02-pitfalls.md 坑 20。
    std::deque<std::string> tx;
    bool                 want_write = false;

    Session() { rx.reserve(1024); }
};

std::mutex                        g_mutex;
std::map<struct lws*, Session*>   g_by_wsi;
std::map<std::string, std::vector<int>> g_rooms;   // room -> peer ids
int                               g_next_id = 1;

Session* find_session(struct lws* wsi);

// 把一个 peer 从房间移除，并通知同房间其他成员
void leave_room(struct lws* wsi, Session* s, const char* reason);

// 通过 wsi 发送一段文本（入队，触发可写回调）
int queue_text(struct lws* wsi, const std::string& msg) {
    Session* s = find_session(wsi);
    if (!s) return -1;
    s->tx.push_back(msg);          // 入队（不覆盖）
    s->want_write = true;
    lws_callback_on_writable(wsi);
    return 0;
}

Session* find_session(struct lws* wsi) {
    auto it = g_by_wsi.find(wsi);
    return it == g_by_wsi.end() ? nullptr : it->second;
}

// 按 room 找对端（1v1 场景最多一个对端）
struct lws* find_peer_wsi(const std::string& room, int exclude_id) {
    for (auto& kv : g_by_wsi) {
        Session* s = kv.second;
        if (s->in_room && s->room == room && s->peer_id != exclude_id) return kv.first;
    }
    return nullptr;
}

int room_member_count(const std::string& room) {
    auto it = g_rooms.find(room);
    return it == g_rooms.end() ? 0 : int(it->second.size());
}

void leave_room(struct lws* wsi, Session* s, const char* reason) {
    if (!s->in_room) return;

    LOGI(kTag, "peer#%d leaving room '%s' (%s)", s->peer_id, s->room.c_str(), reason);

    // 从房间列表移除
    auto& members = g_rooms[s->room];
    members.erase(std::remove(members.begin(), members.end(), s->peer_id), members.end());
    if (members.empty()) g_rooms.erase(s->room);

    // 通知仍在房间里的对端
    struct lws* peer = find_peer_wsi(s->room, s->peer_id);
    if (peer) {
        SignalMsg bye;
        bye.type = SignalType::kPeerLeft;
        bye.room = s->room;
        bye.reason = reason ? reason : "";
        queue_text(peer, bye.to_json());
    }
    s->in_room = false;
}

int handle_join(struct lws* wsi, Session* s, const SignalMsg& m) {
    if (m.room.empty()) {
        SignalMsg err; err.type = SignalType::kError; err.reason = "empty room";
        queue_text(wsi, err.to_json());
        return -1;
    }

    std::lock_guard<std::mutex> lk(g_mutex);

    if (s->in_room) {
        // 已在房间：先退出（允许切换房间）
        leave_room(wsi, s, "switch room");
    }

    if (room_member_count(m.room) >= kMaxPeersPerRoom) {
        LOGW(kTag, "room '%s' full, reject peer#%d", m.room.c_str(), s->peer_id);
        SignalMsg err; err.type = SignalType::kError;
        err.reason = "room full";
        queue_text(wsi, err.to_json());
        return -1;
    }

    s->room = m.room;
    s->in_room = true;
    g_rooms[m.room].push_back(s->peer_id);

    LOGI(kTag, "peer#%d joined room '%s' (%d/%d members)",
         s->peer_id, m.room.c_str(), room_member_count(m.room), kMaxPeersPerRoom);

    // 告知本人入房成功
    SignalMsg joined;
    joined.type = SignalType::kJoined;
    joined.room = m.room;
    joined.peer_id = std::to_string(s->peer_id);
    joined.sdp = std::to_string(room_member_count(m.room));  // 复用 sdp 字段传成员数
    queue_text(wsi, joined.to_json());

    // 若对端已在房间，双向通知"可以开始协商了"
    struct lws* peer = find_peer_wsi(m.room, s->peer_id);
    if (peer) {
        SignalMsg ready;
        ready.type = SignalType::kPeerReady;
        ready.room = m.room;
        ready.peer_id = std::to_string(s->peer_id);
        queue_text(peer, ready.to_json());

        SignalMsg mine = ready;
        mine.peer_id = "peer";
        queue_text(wsi, mine.to_json());
        LOGI(kTag, "room '%s' paired: both peers ready", m.room.c_str());
    }
    return 0;
}

int handle_forward(struct lws* wsi, Session* s, const SignalMsg& m, const char* what) {
    if (!s->in_room) {
        SignalMsg err; err.type = SignalType::kError;
        err.reason = "not in room";
        queue_text(wsi, err.to_json());
        return -1;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    struct lws* peer = find_peer_wsi(s->room, s->peer_id);
    if (!peer) {
        LOGW(kTag, "no peer in room '%s' to forward %s", s->room.c_str(), what);
        SignalMsg err; err.type = SignalType::kError;
        err.reason = "peer not connected";
        queue_text(wsi, err.to_json());
        return -1;
    }

    SignalMsg fwd = m;
    fwd.peer_id = std::to_string(s->peer_id);
    queue_text(peer, fwd.to_json());

    LOGI(kTag, "forwarded %s from peer#%d to room '%s' (%zu bytes)",
         what, s->peer_id, s->room.c_str(), fwd.to_json().size());
    return 0;
}

void on_message(struct lws* wsi, const std::string& text) {
    Session* s = find_session(wsi);
    if (!s) return;

    SignalMsg m;
    if (!SignalMsg::from_json(text, &m)) {
        LOGW(kTag, "peer#%d sent malformed json: %s", s->peer_id, text.c_str());
        SignalMsg err; err.type = SignalType::kError; err.reason = "bad json";
        queue_text(wsi, err.to_json());
        return;
    }

    LOGD(kTag, "recv from peer#%d: type=%s len=%zu",
         s->peer_id, signal_type_name(m.type), text.size());

    switch (m.type) {
        case SignalType::kJoin:
            handle_join(wsi, s, m);
            break;
        case SignalType::kOffer:
            handle_forward(wsi, s, m, "offer");
            break;
        case SignalType::kAnswer:
            handle_forward(wsi, s, m, "answer");
            break;
        case SignalType::kCandidate:
            handle_forward(wsi, s, m, "candidate");
            break;
        case SignalType::kBye:
            leave_room(wsi, s, m.reason.empty() ? "bye" : m.reason.c_str());
            break;
        case SignalType::kPing:
        case SignalType::kStats:
            handle_forward(wsi, s, m, signal_type_name(m.type));
            break;
        default:
            LOGW(kTag, "peer#%d: unsupported message type %s",
                 s->peer_id, signal_type_name(m.type));
            break;
    }
}

// ---------------- libwebsockets 回调 ----------------
int callback_ws(struct lws* wsi, enum lws_callback_reasons reason,
                void* user, void* in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            std::lock_guard<std::mutex> lk(g_mutex);
            Session* s = new Session();
            s->peer_id = g_next_id++;
            s->rx.reserve(4096);
            g_by_wsi[wsi] = s;
            LOGI(kTag, "connection established -> peer#%d (total %zu sessions)",
                 s->peer_id, g_by_wsi.size());
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            Session* s = find_session(wsi);
            if (!s) break;

            // LWS 可能分片投递，需要自己重组（lws_is_final_fragment 判断结尾）
            s->rx.insert(s->rx.end(), (uint8_t*)in, (uint8_t*)in + len);
            if (s->rx.size() > kMaxMsgBytes) {
                LOGW(kTag, "peer#%d message too large (%zu), dropping", s->peer_id, s->rx.size());
                s->rx.clear();
                break;
            }
            if (!lws_is_final_fragment(wsi)) break;

            std::string text(s->rx.begin(), s->rx.end());
            s->rx.clear();
            on_message(wsi, text);
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            Session* s = find_session(wsi);
            if (!s || s->tx.empty()) break;

            // 取队首一条发送（不能一次全发：lws_write 每次只处理一条消息）
            const std::string& msg = s->tx.front();

            // 关键：lws_write 要求缓冲区前面预留 LWS_PRE 字节（协议头空间），
            // 且这段空间必须可写。早期版本直接传 tx 的裸指针，
            // libwebsockets 会往前面写协议头 → 堆越界 → "double free or corruption"。
            // 见 docs/02-pitfalls.md 坑 18。
            std::vector<uint8_t> outbuf(LWS_PRE + msg.size());
            memcpy(outbuf.data() + LWS_PRE, msg.data(), msg.size());

            int n = lws_write(wsi, outbuf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
            if (n < 0) {
                LOGE(kTag, "peer#%d lws_write failed: %d", s->peer_id, n);
            } else {
                LOGD(kTag, "peer#%d sent %d/%zu bytes", s->peer_id, n, msg.size());
            }
            s->tx.pop_front();

            // 队列还有积压就继续请求可写
            if (!s->tx.empty()) lws_callback_on_writable(wsi);
            else s->want_write = false;
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_by_wsi.find(wsi);
            if (it != g_by_wsi.end()) {
                Session* s = it->second;
                leave_room(wsi, s, "disconnected");
                LOGI(kTag, "peer#%d disconnected", s->peer_id);
                delete s;
                g_by_wsi.erase(it);
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

const struct lws_protocols kProtocols[] = {
    {"rtc-signal", callback_ws, 0, 4096, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};

}  // namespace

int main(int argc, char** argv) {
    int port = 8888;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("=== RTC 信令服务器 ===\n");
    printf("  监听端口 : %d\n", port);
    printf("  协议     : WebSocket (text frames, JSON)\n");
    printf("  房间容量 : %d (1v1)\n", kMaxPeersPerRoom);
    printf("  职责     : 房间管理 + offer/answer/candidate 转发（不碰媒体）\n\n");

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.protocols = kProtocols;
    info.gid = -1;
    info.uid = -1;

    struct lws_context* ctx = lws_create_context(&info);
    if (!ctx) {
        fprintf(stderr, "lws_create_context failed (port %d in use?)\n", port);
        return 1;
    }

    LOGI(kTag, "server started on port %d", port);
    while (1) {
        lws_service(ctx, 50);
    }
    lws_context_destroy(ctx);
    return 0;
}
