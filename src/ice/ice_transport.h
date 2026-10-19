#pragma once
// ICE 传输模块 —— 对应 WebRTC 的 ICE / STUN 层
//
// 职责：
//   1. 创建 NiceAgent 并附加到 glib 主循环（用于事件驱动收发）
//   2. 收集本地候选（host / srflx），经信令交换
//   3. 与远端候选做连通性检查，选出可用候选对
//   4. 在选中的候选对上收发媒体数据
//
// 关键设计（与 WebRTC 的对应）：
//   - host 候选：本机网卡地址
//   - srflx 候选：通过 STUN 获得的公网映射（需要 STUN 服务器）
//   - relay 候选：TURN 中继（需要 TURN 服务器，本项目可选配置）
//   - 候选优先级：host > srflx > relay（由 libnice 内部按 type preference 计算）
//
// libnice 的 SDP 辅助 API 让候选交换变得简单：
//   nice_agent_generate_local_sdp()   -> 本地候选的 SDP 片段
//   nice_agent_parse_remote_sdp()     -> 解析远端 SDP 片段
// 本项目正是用它们来实现"类似 WebRTC 的候选交换"。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct _NiceAgent NiceAgent;
struct _GMainLoop;
struct _GMainContext;

namespace rtc {

class IceTransport {
public:
    // 收到媒体数据回调
    using RecvCallback = std::function<void(const uint8_t* data, size_t len)>;
    // 本地候选就绪回调（候选的 SDP 片段，用于经信令发送）
    using CandidateCallback = std::function<void(const std::string& sdp_fragment)>;
    // 连接状态变化回调（true = 已选通候选对，可开始收发）
    using StateCallback = std::function<void(bool connected, const std::string& pair_desc)>;

    struct Config {
        std::string stun_server;          // 例如 "stun.l.google.com"，空则不使用 STUN
        int         stun_port = 19302;
        std::string turn_server;          // 可选
        int         turn_port = 3478;
        std::string turn_user;
        std::string turn_password;
        uint32_t    stream_id = 1;
        bool        controlling = true;   // true=主控方（发起 offer 的一方）
    };

    IceTransport();
    ~IceTransport();

    bool init(const Config& cfg);
    void close();

    void set_on_recv(RecvCallback cb) { on_recv_ = std::move(cb); }
    void set_on_candidate(CandidateCallback cb) { on_candidate_ = std::move(cb); }
    void set_on_state(StateCallback cb) { on_state_ = std::move(cb); }

    // 获取本地候选 SDP 片段（等收集完成后调用）
    std::string local_sdp() const;

    // 解析并设置远端候选（来自信令）
    bool set_remote_sdp(const std::string& sdp_fragment);

    // ------------------------------------------------------------------
    // Trickle ICE —— 候选边收集边发送，不必等收集完成
    //
    // 为什么重要（面试高频）：
    //   非 Trickle 模式要等所有候选收集完（含 STUN 往返，可能几秒）
    //   才开始交换 SDP，建链延迟高。
    //   Trickle ICE 让每个候选一产生就发出去，对端立刻开始连通性检查，
    //   把建链时间从"最慢候选"降到"最先可用的候选"。
    //
    // 本接口对应 WebRTC 的 onicecandidate 事件 + addIceCandidate()。
    // ------------------------------------------------------------------

    // 设置"每产生一个本地候选就回调"的处理器（对应 onicecandidate）
    using TrickleCallback = std::function<void(const std::string& candidate_sdp)>;
    void set_on_trickle_candidate(TrickleCallback cb) { on_trickle_ = std::move(cb); }

    // 增量添加一个远端候选（对应 addIceCandidate）。
    // candidate_sdp 为单个候选的 SDP 片段（a=candidate:... 或含 a= 行）。
    bool add_remote_candidate_sdp(const std::string& candidate_sdp);

    // 已通过 trickle 收集到的本地候选数
    int trickled_count() const { return trickled_count_; }

    // 本地 ICE 凭证（Trickle ICE 时需随 SDP 一起交换，
    // 因为单个候选片段里不含 ufrag/pwd）
    struct Credentials {
        std::string ufrag;
        std::string pwd;
        bool valid() const { return !ufrag.empty() && !pwd.empty(); }
    };
    Credentials local_credentials() const;

    // 设置远端凭证（Trickle ICE 建链的必要步骤）
    void set_remote_credentials(const std::string& ufrag, const std::string& pwd);

    // 发送媒体数据（在候选对选通后有效）
    bool send(const uint8_t* data, size_t len);

    bool connected() const { return connected_; }
    uint64_t rx_count() const { return rx_count_; }
    uint64_t tx_count() const { return tx_count_; }

    // 等待候选收集完成 / 连接建立
    bool wait_gathering_done(int timeout_ms);
    bool wait_connected(int timeout_ms);

    // 当前选中的候选对描述（用于日志与文档）
    std::string selected_pair() const;

private:
    void run_main_loop();
    bool attach_recv();
    void set_local_sdp(const std::string& sdp);
    void mark_gathering_done();
    void mark_connected(const std::string& pair_desc);
    void handle_incoming(const uint8_t* data, size_t len);

    // libnice 的回调必须是 C 函数指针，因此做成类的静态成员，
    // 通过 user_data 传 this（避免使用匿名命名空间的自由函数访问私有成员）。
    //
    // 注意：这里用 unsigned int / char* / void* 等标准类型，
    // 而不是 guint / gchar / gpointer —— 因为头文件不应引入 glib 依赖
    // （glib 类型只是 typedef 别名，ABI 上等价）。见 docs/02-pitfalls.md 坑 21。
    static void cb_gathering_done(NiceAgent* agent, unsigned int stream_id, void* user_data);
    static void cb_new_selected_pair(NiceAgent* agent, unsigned int stream_id,
                                     unsigned int component_id, char* lfoundation,
                                     char* rfoundation, void* user_data);
    static void cb_component_state(NiceAgent* agent, unsigned int stream_id,
                                   unsigned int component_id, unsigned int state,
                                   void* user_data);
    static void cb_new_candidate(NiceAgent* agent, unsigned int stream_id,
                                 unsigned int component_id, char* foundation,
                                 void* user_data);   // Trickle ICE
    static void cb_recv(NiceAgent* agent, unsigned int stream_id, unsigned int component_id,
                        unsigned int len, char* buf, void* user_data);

    Config cfg_{};
    NiceAgent* agent_ = nullptr;
    _GMainLoop*  loop_ = nullptr;
    _GMainContext* ctx_ = nullptr;   // agent 与主循环共用的上下文（关键）
    std::thread  loop_thread_;

    RecvCallback      on_recv_;
    CandidateCallback on_candidate_;
    StateCallback     on_state_;
    TrickleCallback   on_trickle_;      // Trickle ICE：每个新候选的回调
    int               trickled_count_ = 0;

    std::atomic<bool> gathering_done_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    mutable std::mutex      state_mutex_;
    std::condition_variable state_cv_;

    std::string local_sdp_;
    std::string selected_pair_;

    std::atomic<uint64_t> rx_count_{0};
    std::atomic<uint64_t> tx_count_{0};
};

}  // namespace rtc
