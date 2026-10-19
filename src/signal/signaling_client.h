#pragma once
// WebSocket 信令客户端
//
// 职责：
//   1. 连接信令服务器，收发 JSON 信令消息
//   2. 提供线程安全的消息回调（libwebsockets 回调在它自己的线程里）
//   3. 处理发送队列（业务线程投递 → LWS 线程发送）
//
// 设计要点（对应 WebRTC 的"信令面与媒体面分离"）：
//   信令走 TCP/WebSocket（可靠、有序），媒体走 SRTP/UDP（低延迟、可丢）。
//   两者物理分离，互不干扰。
//
// 使用方式：
//   SignalingClient c;
//   c.set_on_message([](const SignalMsg& m){ ... });
//   c.connect("ws://127.0.0.1:8888");
//   c.send(SignalMsg::make_join("room"));
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "protocol/signaling_msg.h"

struct lws_context;
struct lws;

namespace rtc {

class SignalingClient {
public:
    using MessageCallback = std::function<void(const SignalMsg&)>;
    using StateCallback   = std::function<void(bool connected)>;

    SignalingClient();
    ~SignalingClient();

    // 设置消息回调（收到服务器/对端信令时调用，在 LWS 线程上下文）
    void set_on_message(MessageCallback cb);
    // 设置连接状态回调（连接建立/断开时调用）
    void set_on_state(StateCallback cb);

    // 连接服务器。url 形如 "ws://127.0.0.1:8888"。非阻塞，立即返回。
    bool connect(const std::string& url);
    // 断开并清理
    void disconnect();

    // 发送一条信令（线程安全，可从任意线程调用）
    bool send(const SignalMsg& msg);

    bool connected() const { return connected_; }

    // 等待连接建立（超时返回 false）
    bool wait_connected(int timeout_ms);

    // 接收统计
    uint64_t rx_count() const { return rx_count_; }
    uint64_t tx_count() const { return tx_count_; }

    // 供 LWS 回调访问的内部接口（不要直接调用）
    void handle_incoming(const std::string& text);
    void handle_established();
    void handle_closed();
    void set_wsi(lws* wsi) { wsi_ = wsi; }   // LWS 回调建立连接时登记句柄
    void handle_closed_on_error();
    bool pop_outgoing(std::string* out);
    bool has_outgoing() const;

    // 接收重组缓冲（LWS 可能分片投递一条消息）。
    // 必须是 C++ 成员而非 lws 的 per-session user data——后者是裸内存，
    // 不会构造 std::string。见 docs/02-pitfalls.md 坑 17。
    std::string& rx_buffer() { return rx_buffer_; }
    size_t       max_rx_buffer() const { return 64 * 1024; }

    // 有数据入队时请求 libwebsockets 触发可写回调。
    void request_writable();

    const std::string& path() const { return path_; }

private:
    void service_loop();

    MessageCallback on_message_;
    StateCallback   on_state_;

    // 发送队列（业务线程写，LWS 线程读）
    std::mutex               tx_mutex_;
    std::deque<std::string>  tx_queue_;

    // 连接状态
    std::mutex              state_mutex_;
    std::condition_variable state_cv_;
    std::atomic<bool>       connected_{false};
    std::atomic<bool>       want_connect_{false};
    std::atomic<bool>       running_{false};

    std::thread       service_thread_;
    lws_context*      ctx_ = nullptr;
    lws*              wsi_ = nullptr;   // 当前连接句柄（established 时赋值）
    std::string       host_;
    int               port_ = 0;
    std::string       path_ = "/";

    std::atomic<uint64_t> rx_count_{0};
    std::atomic<uint64_t> tx_count_{0};

    std::string rx_buffer_;   // 见 rx_buffer() 说明
};

}  // namespace rtc
