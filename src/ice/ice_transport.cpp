#include "ice/ice_transport.h"

#include <glib.h>
#include <nice/agent.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>

#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "ice";
// libnice 的 component 编号：1 = RTP，2 = RTCP。
// 本项目用 rtcp-mux 思路，只用 component 1 承载全部媒体与控制
// （与 WebRTC 现代的 BUNDLE / rtcp-mux 一致）。
constexpr guint kComponentRtp = 1;

// 把 hostname（或已是 IP 的字符串）解析为 **IPv4** 点分十进制字符串。
//
// 为什么必须自己做这个转换：
//   1. libnice 0.1.16 的 stun-server 属性只接受 IP，不接受域名；
//   2. getaddrinfo 默认可能返回 IPv6（AF_INET6）排在前面，
//      而很多环境（如 WSL）IPv6 出网不通 → STUN 请求重传超时、srflx 永远收集不到。
// 因此显式指定 AF_INET，只取 IPv4。
std::string resolve_ipv4(const std::string& host) {
    // 已经是 IPv4 字面量则直接用
    struct in_addr tmp;
    if (inet_pton(AF_INET, host.c_str(), &tmp) == 1) return host;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          // ★ 只要 IPv4
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";

    char buf[INET_ADDRSTRLEN] = {0};
    auto* sa = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    const char* ok = inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return ok ? std::string(buf) : std::string();
}
}  // namespace

IceTransport::IceTransport() = default;
IceTransport::~IceTransport() { close(); }

// ---------------- 内部状态更新 ----------------
void IceTransport::set_local_sdp(const std::string& sdp) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    local_sdp_ = sdp;
}

void IceTransport::mark_gathering_done() {
    gathering_done_ = true;
    state_cv_.notify_all();
}

void IceTransport::mark_connected(const std::string& pair_desc) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (!pair_desc.empty()) selected_pair_ = pair_desc;
        if (connected_) return;      // 已通知过就不重复
        connected_ = true;
    }
    state_cv_.notify_all();
    if (on_state_) on_state_(true, pair_desc);
}

void IceTransport::handle_incoming(const uint8_t* data, size_t len) {
    rx_count_++;
    if (on_recv_) on_recv_(data, len);
}

// ---------------- libnice 静态回调 ----------------
void IceTransport::cb_gathering_done(NiceAgent* agent, unsigned int stream_id, void* user_data) {
    auto* self = static_cast<IceTransport*>(user_data);
    LOGI(kTag, "candidate gathering done (stream=%u)", stream_id);

    // 本地候选的 SDP 片段 —— 这就是要经信令发给对端的内容。
    // 对应 WebRTC 里 localDescription 中的 a=candidate 行集合。
    gchar* sdp = nice_agent_generate_local_sdp(agent);
    if (sdp) {
        self->set_local_sdp(sdp);
        g_free(sdp);
        LOGI(kTag, "local candidate sdp (%zu bytes):\n%s",
             self->local_sdp_.size(), self->local_sdp_.c_str());
    } else {
        LOGW(kTag, "generate_local_sdp returned NULL");
    }

    self->mark_gathering_done();
    if (self->on_candidate_ && !self->local_sdp_.empty()) {
        self->on_candidate_(self->local_sdp_);
    }
}

void IceTransport::cb_new_selected_pair(NiceAgent* agent, unsigned int stream_id,
                                        unsigned int component_id, char* lfoundation,
                                        char* rfoundation, void* user_data) {
    auto* self = static_cast<IceTransport*>(user_data);
    const std::string desc = std::string(lfoundation ? lfoundation : "?") + " <-> " +
                             std::string(rfoundation ? rfoundation : "?");
    LOGI(kTag, "new selected pair: %s (component=%u)", desc.c_str(), component_id);
    self->mark_connected(desc);
}

void IceTransport::cb_component_state(NiceAgent* agent, unsigned int stream_id,
                                      unsigned int component_id, unsigned int state,
                                      void* user_data) {
    auto* self = static_cast<IceTransport*>(user_data);
    const char* s = "unknown";
    switch (state) {
        case NICE_COMPONENT_STATE_DISCONNECTED: s = "DISCONNECTED"; break;
        case NICE_COMPONENT_STATE_GATHERING:    s = "GATHERING";    break;
        case NICE_COMPONENT_STATE_CONNECTING:   s = "CONNECTING";   break;
        case NICE_COMPONENT_STATE_CONNECTED:    s = "CONNECTED";    break;
        case NICE_COMPONENT_STATE_READY:        s = "READY";        break;
        case NICE_COMPONENT_STATE_FAILED:       s = "FAILED";       break;
        default: break;
    }
    LOGI(kTag, "component state -> %s (component=%u)", s, component_id);

    // READY = 已有可用候选对，可以传媒体
    if (state == NICE_COMPONENT_STATE_READY) {
        self->mark_connected("");
    }
}

void IceTransport::cb_recv(NiceAgent* agent, unsigned int stream_id, unsigned int component_id,
                           unsigned int len, char* buf, void* user_data) {
    auto* self = static_cast<IceTransport*>(user_data);
    self->handle_incoming(reinterpret_cast<uint8_t*>(buf), len);
}

// Trickle ICE：每产生一个新本地候选就立刻回调出去。
//
// 对应 WebRTC 的 onicecandidate 事件——候选一产生就发信令，
// 不等收集完成。这能把建链时间从"最慢候选"缩短到"最先可用候选"。
void IceTransport::cb_new_candidate(NiceAgent* agent, unsigned int stream_id,
                                    unsigned int component_id, char* foundation,
                                    void* user_data) {
    auto* self = static_cast<IceTransport*>(user_data);

    // 用 libnice 的 API 把这个候选转成 SDP 片段（a=candidate:...）
    GSList* cands = nice_agent_get_local_candidates(agent, stream_id, component_id);
    if (!cands) return;

    for (GSList* it = cands; it; it = it->next) {
        auto* c = static_cast<NiceCandidate*>(it->data);
        gchar* sdp = nice_agent_generate_local_candidate_sdp(agent, c);
        if (!sdp) continue;

        self->trickled_count_++;
        LOGI(kTag, "trickle candidate #%d: %s", self->trickled_count_,
             sdp[0] ? sdp : "?");

        if (self->on_trickle_) {
            self->on_trickle_(std::string(sdp));
        }
        g_free(sdp);
    }
    g_slist_free_full(cands, reinterpret_cast<GDestroyNotify>(nice_candidate_free));
}

// ---------------- 初始化 ----------------
bool IceTransport::init(const Config& cfg) {
    if (agent_) {
        LOGW(kTag, "already initialized");
        return true;
    }
    cfg_ = cfg;

    // ★ 关键设计：agent 与主循环必须共用同一个 GMainContext。
    //
    // 之前的实现用 nice_agent_new(g_main_context_default(), ...) 创建 agent，
    // 却用 g_main_loop_new(NULL, ...) 跑主循环 —— 后者迭代的是 **thread-default**
    // 上下文，与 agent 所在的 default 上下文不是同一个。
    // 结果：libnice 的 socket source 挂在 default 上下文上，无人迭代，
    // STUN 响应包到达内核缓冲区却永远不会被 agent 读到 →
    // 表现为"请求发出、响应也回来了，但持续重传直至超时"。
    //
    // tcpdump 证据（这正是定位该问题的关键）：
    //   192.168.0.197.45878 > 74.125.250.129.19302: UDP, length 20   ← 请求发出
    //   74.125.250.129.19302 > 192.168.0.197.45878: UDP, length 32   ← 响应回来
    //   libnice 日志: STUN transaction retransmitted                 ← 却收不到
    // 见 docs/02-pitfalls.md 坑 23。
    ctx_ = g_main_context_new();
    agent_ = nice_agent_new(ctx_, NICE_COMPATIBILITY_RFC5245);
    if (!agent_) {
        LOGE(kTag, "nice_agent_new failed");
        g_main_context_unref(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // STUN 服务器配置 —— 这是获得 srflx（公网映射）候选的唯一途径。
    //
    // ★ 重要：libnice 0.1.16 的 stun-server 属性文档明确写着
    //   "The IP address (not the hostname) of the STUN server to use"
    // 也就是**只接受 IP，不接受域名**。传域名会被静默忽略（不报错、不收集 srflx）。
    // 因此这里做一次显式的 hostname -> IPv4 解析。
    //
    // 另外必须强制 IPv4：gethostbyname 可能优先返回 IPv6，
    // 而 WSL 的 IPv6 出网不通，会导致 STUN 请求不断重传直至超时。
    // 见 docs/02-pitfalls.md 坑 24。
    if (!cfg_.stun_server.empty()) {
        std::string stun_ip = resolve_ipv4(cfg_.stun_server);
        if (stun_ip.empty()) {
            LOGW(kTag, "cannot resolve IPv4 for STUN '%s', STUN disabled",
                 cfg_.stun_server.c_str());
        } else {
            g_object_set(G_OBJECT(agent_),
                         "stun-server", stun_ip.c_str(),
                         "stun-server-port", guint(cfg_.stun_port),
                         nullptr);
            LOGI(kTag, "STUN configured: %s (%s):%d",
                 cfg_.stun_server.c_str(), stun_ip.c_str(), cfg_.stun_port);
        }
    } else {
        LOGI(kTag, "no STUN server (only host candidates)");
    }

    // controlling / controlled 角色决定了连通性检查的提名权。
    // WebRTC 里由 offer 方（主叫）担任 controlling。
    g_object_set(G_OBJECT(agent_), "controlling-mode",
                 cfg_.controlling ? TRUE : FALSE, nullptr);
    LOGI(kTag, "role: %s", cfg_.controlling ? "controlling" : "controlled");

    // 1v1 通话：1 个 stream，1 个 component（rtcp-mux 思路）
    if (!nice_agent_add_stream(agent_, 1)) {
        LOGE(kTag, "nice_agent_add_stream failed");
        g_object_unref(agent_);
        agent_ = nullptr;
        return false;
    }

    // 连接信号：候选收集完成 / 选中候选对 / 组件状态 / 数据到达
    g_signal_connect(G_OBJECT(agent_), "candidate-gathering-done",
                     G_CALLBACK(cb_gathering_done), this);
    g_signal_connect(G_OBJECT(agent_), "new-selected-pair",
                     G_CALLBACK(cb_new_selected_pair), this);
    g_signal_connect(G_OBJECT(agent_), "component-state-changed",
                     G_CALLBACK(cb_component_state), this);
    // Trickle ICE：候选一产生就回调（对应 onicecandidate）
    g_signal_connect(G_OBJECT(agent_), "new-candidate",
                     G_CALLBACK(cb_new_candidate), this);

    // glib 主循环：用我们自己的 ctx_（与 agent 共用），
    // 这样 libnice 的 socket source 才能被真正迭代。见上方 ★ 说明。
    loop_ = g_main_loop_new(ctx_, FALSE);
    running_ = true;
    loop_thread_ = std::thread([this] { run_main_loop(); });

    // 在 gather_candidates 之前附加接收回调：
    // STUN 响应经 libnice 的接收路径回到 agent，必须先接上。
    if (!attach_recv()) {
        LOGE(kTag, "attach_recv failed");
        return false;
    }
    LOGI(kTag, "recv attached before gathering");

    // 开始收集候选（异步）
    if (!nice_agent_gather_candidates(agent_, cfg_.stream_id)) {
        LOGE(kTag, "nice_agent_gather_candidates failed");
        return false;
    }
    LOGI(kTag, "candidate gathering started (stream=%u)", cfg_.stream_id);
    return true;
}

void IceTransport::run_main_loop() {
    g_main_loop_run(loop_);
    LOGI(kTag, "main loop exited");
}

bool IceTransport::attach_recv() {
    // 用 ctx_（agent 自己的上下文）附加接收。
    // 传 NULL 也可以（libnice 会用 agent 的默认上下文），
    // 但显式传 ctx_ 更清晰、也更不容易出错。见 docs/02-pitfalls.md 坑 23。
    return nice_agent_attach_recv(agent_, cfg_.stream_id, kComponentRtp,
                                  ctx_, cb_recv, this) != FALSE;
}

void IceTransport::close() {
    if (!running_ && !agent_) return;
    running_ = false;

    if (loop_) g_main_loop_quit(loop_);
    if (loop_thread_.joinable()) loop_thread_.join();

    if (loop_) { g_main_loop_unref(loop_); loop_ = nullptr; }
    if (agent_) { g_object_unref(agent_); agent_ = nullptr; }
    if (ctx_) { g_main_context_unref(ctx_); ctx_ = nullptr; }

    connected_ = false;
    gathering_done_ = false;
    LOGI(kTag, "closed");
}

// ---------------- 远端候选 ----------------
std::string IceTransport::local_sdp() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return local_sdp_;
}

bool IceTransport::set_remote_sdp(const std::string& sdp_fragment) {
    if (!agent_ || sdp_fragment.empty()) return false;

    // 解析远端候选（对应 WebRTC 的 setRemoteDescription + addIceCandidate）。
    // libnice 的 parse_remote_sdp 会解析 a=ice-ufrag / a=ice-pwd / a=candidate 行，
    // 并**自动设置远端候选 + 启动连通性检查**，不需要额外调用其他 API。
    //
    // 注意：不要尝试调用 nice_agent_set_remote_candidates_done()——
    // 该函数在 libnice 0.1.16 中不存在（那是更新版本 API）。
    // 具体版本差异见 docs/02-pitfalls.md 坑 22。
    if (!nice_agent_parse_remote_sdp(agent_, sdp_fragment.c_str())) {
        LOGE(kTag, "parse_remote_sdp failed");
        return false;
    }
    LOGI(kTag, "remote candidates parsed (%zu bytes)", sdp_fragment.size());

    attach_recv();
    LOGI(kTag, "connectivity checks started (auto-triggered by parse_remote_sdp)");
    return true;
}

IceTransport::Credentials IceTransport::local_credentials() const {
    Credentials c;
    if (!agent_) return c;
    gchar* ufrag = nullptr;
    gchar* pwd = nullptr;
    nice_agent_get_local_credentials(agent_, cfg_.stream_id, &ufrag, &pwd);
    if (ufrag) { c.ufrag = ufrag; g_free(ufrag); }
    if (pwd)   { c.pwd = pwd;     g_free(pwd); }
    return c;
}

void IceTransport::set_remote_credentials(const std::string& ufrag, const std::string& pwd) {
    if (!agent_) return;
    nice_agent_set_remote_credentials(agent_, cfg_.stream_id, ufrag.c_str(), pwd.c_str());
    LOGI(kTag, "remote credentials set");
}

// 增量添加一个远端候选（Trickle ICE 的 addIceCandidate）。
//
// 与 set_remote_sdp 的区别：
//   set_remote_sdp        一次性把整段 SDP 解析进来（非 trickle 模式）
//   add_remote_candidate_sdp  来一个候选就加一个，立即开始连通性检查
bool IceTransport::add_remote_candidate_sdp(const std::string& candidate_sdp) {
    if (!agent_ || candidate_sdp.empty()) return false;

    // libnice 的 parse_remote_candidate_sdp 解析**单个**候选，
    // 返回 NiceCandidate*（不是 GSList —— 这点容易搞错，
    // 因为旁边的 parse_remote_stream_sdp 返回的才是 GSList）。
    // 见 docs/02-pitfalls.md 坑 31。
    NiceCandidate* c = nice_agent_parse_remote_candidate_sdp(
        agent_, cfg_.stream_id, candidate_sdp.c_str());
    if (!c) {
        LOGW(kTag, "parse_remote_candidate_sdp failed for: %.60s", candidate_sdp.c_str());
        return false;
    }

    // 包成单元素 GSList 提交给 agent
    GSList* list = g_slist_append(nullptr, c);
    const int added = nice_agent_set_remote_candidates(
        agent_, cfg_.stream_id, kComponentRtp, list);
    g_slist_free(list);
    nice_candidate_free(c);

    if (added < 0) {
        LOGE(kTag, "set_remote_candidates failed: %d", added);
        return false;
    }
    LOGI(kTag, "trickle: added %d remote candidate(s)", added);
    return true;
}

// ---------------- 媒体收发 ----------------
bool IceTransport::send(const uint8_t* data, size_t len) {
    if (!agent_ || !connected_) return false;

    gint sent = nice_agent_send(agent_, cfg_.stream_id, kComponentRtp,
                                guint(len), reinterpret_cast<const gchar*>(data));
    if (sent < 0) {
        LOGE(kTag, "nice_agent_send failed: %d", sent);
        return false;
    }
    tx_count_++;
    return true;
}

bool IceTransport::wait_gathering_done(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_mutex_);
    return state_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [this] { return gathering_done_.load(); });
}

bool IceTransport::wait_connected(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_mutex_);
    return state_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [this] { return connected_.load(); });
}

std::string IceTransport::selected_pair() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return selected_pair_;
}

}  // namespace rtc
