#include "signal/signaling_client.h"

#include <cstdlib>
#include <cstring>

#include <libwebsockets.h>

#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "signal_cli";
constexpr int  kServiceTimeoutMs = 50;
constexpr size_t kMaxRxBuffer = 64 * 1024;

// libwebsockets 协议回调需要一个 C 风格函数，
// 通过 user 指针把 C++ 对象传进来。
int client_callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);

// libwebsockets 的 per-session user data 是它自己用 malloc 分配的**裸内存**，
// 不会调用 C++ 构造函数（也不会调析构）。
// 因此这里**只能放 POD 成员**——早期版本放了一个 std::string，
// 其内部指针是未初始化野值，第一次 clear() 就段错误。
// 见 docs/02-pitfalls.md 坑 17。
struct PerSessionData {
    SignalingClient* client = nullptr;   // POD：指针，lws 会零初始化
};
}  // namespace

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    disconnect();
}

void SignalingClient::set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
void SignalingClient::set_on_state(StateCallback cb) { on_state_ = std::move(cb); }

bool SignalingClient::connect(const std::string& url) {
    if (running_) {
        LOGW(kTag, "already running");
        return true;
    }

    // 解析 ws://host:port/path
    std::string s = url;
    if (s.rfind("ws://", 0) == 0) s = s.substr(5);
    else {
        LOGE(kTag, "unsupported url scheme (expect ws://): %s", url.c_str());
        return false;
    }

    const size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    path_ = (slash == std::string::npos) ? "/" : s.substr(slash);

    const size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        host_ = hostport;
        port_ = 80;
    } else {
        host_ = hostport.substr(0, colon);
        port_ = atoi(hostport.c_str() + colon + 1);
    }

    LOGI(kTag, "connecting to ws://%s:%d%s", host_.c_str(), port_, path_.c_str());

    running_ = true;
    service_thread_ = std::thread([this] { service_loop(); });
    return true;
}

void SignalingClient::disconnect() {
    if (!running_) return;
    running_ = false;
    if (service_thread_.joinable()) service_thread_.join();

    std::lock_guard<std::mutex> lk(state_mutex_);
    connected_ = false;
    ctx_ = nullptr;
    LOGI(kTag, "disconnected");
}

bool SignalingClient::send(const SignalMsg& msg) {
    std::string text = msg.to_json();
    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        tx_queue_.push_back(std::move(text));
    }
    tx_count_++;
    LOGD(kTag, "queued %s (%zu in queue)", signal_type_name(msg.type), tx_queue_.size());
    request_writable();
    return true;
}

void SignalingClient::request_writable() {
    // lws_callback_on_writable 是线程安全的，可从任意线程调用
    if (wsi_) lws_callback_on_writable(wsi_);
}

bool SignalingClient::has_outgoing() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(tx_mutex_));
    return !tx_queue_.empty();
}

bool SignalingClient::wait_connected(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_mutex_);
    return state_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [this] { return connected_.load(); });
}

bool SignalingClient::pop_outgoing(std::string* out) {
    std::lock_guard<std::mutex> lk(tx_mutex_);
    if (tx_queue_.empty()) return false;
    *out = std::move(tx_queue_.front());
    tx_queue_.pop_front();
    return true;
}

void SignalingClient::handle_incoming(const std::string& text) {
    SignalMsg msg;
    if (!SignalMsg::from_json(text, &msg)) {
        LOGW(kTag, "malformed json from server: %s", text.c_str());
        return;
    }
    rx_count_++;
    LOGI(kTag, "recv %s%s", signal_type_name(msg.type),
         msg.peer_id.empty() ? "" : (" from peer " + msg.peer_id).c_str());
    if (on_message_) on_message_(msg);
}

void SignalingClient::handle_established() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        connected_ = true;
    }
    state_cv_.notify_all();
    LOGI(kTag, "connection established");
    if (on_state_) on_state_(true);
}

void SignalingClient::handle_closed() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        connected_ = false;
    }
    state_cv_.notify_all();
    LOGI(kTag, "connection closed");
    if (on_state_) on_state_(false);
}

void SignalingClient::service_loop() {
    // 协议名必须与服务器端一致（服务器用 "rtc-signal"）。
    // 早期版本这里写成 "rtc-signal-cli"，导致握手失败：
    //   服务器日志: No supported protocol "rtc-signal-cli"
    // libwebsockets 的 subprotocol 协商要求两端名字完全相同。
    // 见 docs/02-pitfalls.md 坑 16。
    static const lws_protocols protocols[] = {
        {"rtc-signal", client_callback, sizeof(PerSessionData), 4096, 0, nullptr, 0},
        {nullptr, nullptr, 0, 0, 0, nullptr, 0},
    };

    lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;   // 客户端不监听
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    lws_context* ctx = lws_create_context(&info);
    if (!ctx) {
        LOGE(kTag, "lws_create_context failed");
        running_ = false;
        return;
    }
    ctx_ = ctx;

    // 建立客户端连接。
    // 关键：把 C++ 对象通过 opaque_user_data 传进回调。
    // 注意不能用 per-session user 指针（connect 返回前 session 尚未创建），
    // 而 opaque_user_data 在整个连接生命周期内都有效。见 docs/02-pitfalls.md 坑 15。
    lws_client_connect_info ci;
    memset(&ci, 0, sizeof(ci));
    ci.context = ctx;
    ci.address = host_.c_str();
    ci.port = port_;
    ci.path = path_.c_str();
    ci.host = host_.c_str();
    ci.origin = host_.c_str();
    ci.protocol = protocols[0].name;
    ci.local_protocol_name = protocols[0].name;
    ci.opaque_user_data = this;

    lws* wsi = lws_client_connect_via_info(&ci);
    if (!wsi) {
        LOGE(kTag, "lws_client_connect_via_info failed (server not running?)");
        lws_context_destroy(ctx);
        ctx_ = nullptr;
        running_ = false;
        return;
    }
    wsi_ = wsi;

    // 服务循环
    while (running_) {
        lws_service(ctx, kServiceTimeoutMs);
    }

    lws_context_destroy(ctx);
    ctx_ = nullptr;
}

// ---------------- libwebsockets 回调 ----------------
namespace {
int client_callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    auto* data = static_cast<PerSessionData*>(user);

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            auto* client = static_cast<SignalingClient*>(lws_get_opaque_user_data(wsi));
            if (data) data->client = client;
            if (client) {
                client->set_wsi(wsi);
                client->rx_buffer().clear();
                client->handle_established();
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            auto* client = static_cast<SignalingClient*>(lws_get_opaque_user_data(wsi));
            if (!client) break;

            std::string& rx = client->rx_buffer();
            rx.append(static_cast<char*>(in), len);
            if (rx.size() > client->max_rx_buffer()) {
                LOGW(kTag, "rx buffer overflow, dropping %zu bytes", rx.size());
                rx.clear();
                break;
            }
            if (!lws_is_final_fragment(wsi)) break;

            std::string text = std::move(rx);
            rx.clear();
            client->handle_incoming(text);
            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            auto* client = static_cast<SignalingClient*>(lws_get_opaque_user_data(wsi));
            if (!client) break;

            std::string out;
            if (!client->pop_outgoing(&out)) break;   // 没有待发数据

            // libwebsockets 要求前 LWS_PRE 字节预留（用于协议头）
            std::vector<uint8_t> buf(LWS_PRE + out.size());
            memcpy(buf.data() + LWS_PRE, out.data(), out.size());
            int n = lws_write(wsi, buf.data() + LWS_PRE, out.size(), LWS_WRITE_TEXT);
            if (n < 0) {
                LOGE(kTag, "lws_write failed: %d", n);
                break;
            }
            LOGD(kTag, "sent %d/%zu bytes", n, out.size());

            // 队列还有积压就继续请求可写（否则等下次 send 时再请求）
            if (client->has_outgoing()) lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            LOGE(kTag, "connection error: %s", in ? static_cast<char*>(in) : "unknown");
            if (auto* c = static_cast<SignalingClient*>(lws_get_opaque_user_data(wsi))) {
                c->handle_closed();
            }
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
        case LWS_CALLBACK_CLOSED:
            if (auto* c = static_cast<SignalingClient*>(lws_get_opaque_user_data(wsi))) {
                c->handle_closed();
            }
            break;

        default:
            break;
    }
    return 0;
}
}  // namespace

}  // namespace rtc
