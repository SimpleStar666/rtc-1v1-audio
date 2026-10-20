#pragma once
// 视频管线 —— 对应 WebRTC 的视频收发链路
//
// 与音频管线（AudioPipeline）的结构对称，但视频有三个**音频没有的**问题：
//
// ① 包太大必须分片
//    关键帧动辄几十 KB，而 UDP 单包实用上限约 1200 字节（避免 IP 分片）。
//    所以一帧要拆成多个 RTP 包发送，接收端再拼回去。
//    （WebRTC 用 RTP 的 FU-A / VP8 payload descriptor 做这件事，
//      本项目用简化格式：[frame_id][frag_idx][frag_count][is_key] + 数据）
//
// ② 丢一个包可能导致整帧不可用
//    音频丢包可以 PLC 补；视频丢一个分片，整帧就残缺 → 必须整帧丢弃。
//    更严重的是**丢关键帧会让后续所有 P 帧都无法解码**（参考帧缺失）。
//
// ③ 必须有关键帧请求（PLI / FIR）
//    接收端一旦发现"解码失败且无法恢复"，就要向发送端请求关键帧。
//    WebRTC 用 RTCP PLI/FIR 反馈；本项目用自定义控制消息。
//    这是视频独有、也是面试最爱问的机制。
//
// 本模块职责：
//   发送：采集 → VP8 编码 → 分片 → 交给传输层（回调）
//   接收：分片重组 → 丢包检测 → VP8 解码 → 送播放
//         并在此过程中决定"何时请求关键帧"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "media/video_device.h"
#include "media/vp8_codec.h"
#include "protocol/packet.h"

namespace rtc {

// 视频分片头（放在 PacketHeader 之后的载荷里）
//
// 为什么需要它：音频包一帧 = 一个包，视频一帧 = N 个包，
// 接收端必须知道"这个包属于哪一帧的第几片、总共有几片"。
//
// 布局（8 字节）：
//   [0..1]  frame_id     帧序号（16bit，回绕）
//   [2..3]  frag_index   本片在帧内的序号（0-based）
//   [3..3]  见下
//   [4..5]  frag_count   本帧总片数
//   [6]     flags        bit0 = 关键帧
//   [7]     保留
struct VideoFragmentHeader {
    uint16_t frame_id   = 0;
    uint16_t frag_index = 0;
    uint16_t frag_count = 0;
    bool     is_keyframe = false;

    static constexpr size_t kSize = 8;
};

class VideoPipeline {
public:
    // 发送出口：把一个分片交给传输层（由 call_session 接到 ICE+SRTP）
    using SendFn = std::function<void(const PacketHeader& h,
                                      const std::vector<uint8_t>& payload)>;
    // 关键帧请求出口：接收端发现需要关键帧时，通知传输层发控制消息给对端
    using KeyframeRequestFn = std::function<void()>;

    struct Config {
        int  session_id = 2;      // 视频用独立 SSRC（音频是 1）
        int  width  = 320;
        int  height = 240;
        int  fps    = 15;
        int  bitrate_kbps = 400;
        int  keyframe_interval = 30;   // 每 N 帧一个关键帧
        size_t max_fragment_payload = 1100;   // 单包最大载荷（留头空间）
        bool sink_frames = false;     // 是否落盘解码后的帧（PPM）
        std::string sink_dir;
    };

    VideoPipeline();
    ~VideoPipeline();

    bool init(const Config& cfg, VirtualVideoDevice* device);
    void set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }
    void set_keyframe_request_fn(KeyframeRequestFn fn) { kf_req_fn_ = std::move(fn); }

    void start();
    void stop();

    // 接收：收到一个视频分片（由传输层解密解析后调用）
    void on_video_fragment(const PacketHeader& h, const std::vector<uint8_t>& payload);

    // 收到对端的关键帧请求（对应 RTCP PLI）
    void on_keyframe_request();

    // 主动请求对端发关键帧（对应接收端主动发 PLI）。
    //
    // 为什么需要"主动"而不是"等失败再请求"：
    //   接收端刚启动时，对端可能已经在发 P 帧了，而 P 帧没有参考帧必然解不出。
    //   如果只在"解码失败"时请求，会有 1 个 RTT 的纯丢帧窗口。
    //   WebRTC 的做法是：接收端一开始就发 PLI（setRemoteDescription 后）。
    //   见 docs/02-pitfalls.md 坑 47。
    void request_keyframe_now();

    // 运行时码率调整（拥塞控制）
    void set_target_bitrate_kbps(int kbps);

    struct Report {
        uint64_t frames_captured = 0;
        uint64_t frames_encoded  = 0;
        uint64_t keyframes_sent  = 0;
        uint64_t fragments_sent  = 0;
        uint64_t bytes_sent      = 0;
        uint64_t fragments_recv  = 0;
        uint64_t frames_recv     = 0;   // 完整重组出的帧
        uint64_t frames_decoded  = 0;
        uint64_t frames_dropped  = 0;   // 因缺片/解码失败丢弃
        uint64_t keyframe_requests = 0; // 发出过多少次关键帧请求
        uint64_t keyframes_recv  = 0;
        double   actual_kbps = 0;
        double   fps_out = 0;
        double   fps_in  = 0;
        size_t   pending_frames = 0;    // 正在重组中的帧数
    };
    Report report() const;

private:
    void send_loop();
    void assemble_and_decode(uint16_t frame_id);
    // 统一的"请求关键帧"入口（自带限流，见 .cpp 里的说明）
    void request_keyframe_throttled();

    Config cfg_;
    VirtualVideoDevice* device_ = nullptr;
    Vp8Codec codec_;
    Vp8Codec decoder_;

    SendFn send_fn_;
    KeyframeRequestFn kf_req_fn_;

    std::thread send_thread_;
    std::atomic<bool> running_{false};

    // 发送侧统计
    mutable std::mutex stat_mutex_;
    Report rep_;
    int64_t send_start_ms_ = 0;
    uint16_t next_frame_id_ = 0;
    int64_t last_kf_req_ms_ = 0;

    // 是否已经成功解出过至少一个关键帧。
    //
    // 为什么需要它：接收端刚启动时完全没有画面，此时对关键帧的需求是**紧急**的。
    // 而启动时主动发的第一个 PLI 常常"白发"——对端管线可能还没就绪，
    // 或者信令还没转过去。如果此时还按 300ms 限流，真正有用的那次请求
    // 会被压住，白白多等 3~10 帧的纯黑屏。
    //
    // 实测：加上这个"首帧前更激进"的策略后，
    // 加压测试失败轮次从 4/10 降到 1/10（残留的是网络抖动）。
    // 见 docs/02-pitfalls.md 坑 47。
    bool first_keyframe_decoded_ = false;

    // 接收侧重组的帧
    struct PendingFrame {
        uint16_t frag_count = 0;
        bool     is_keyframe = false;
        std::set<uint16_t> got;
        std::vector<uint8_t> data;      // 按片序拼接
        std::vector<uint32_t> frag_len; // 每片长度（拼接用）
        int64_t  first_ms = 0;
    };
    mutable std::mutex rx_mutex_;
    std::map<uint16_t, PendingFrame> pending_;
    std::set<uint16_t> lost_keyframe_until_;   // 这些 frame_id 之前都不可解码

    // 码率统计窗口
    int64_t kbps_window_ms_ = 0;
    uint64_t kbps_window_bytes_ = 0;
};

}  // namespace rtc
