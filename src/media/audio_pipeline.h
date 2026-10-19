#pragma once
// 音频管线 —— 把采集 → 编码 → 发送 和 接收 → 抖动缓冲 → 解码 → 播放 串起来
//
// 这是全景中的"媒体面主干"，对应 WebRTC 里的：
//   发送方向：ADM 采集 -> APM 前处理 -> Opus 编码 -> RTP 打包 -> Pacer
//   接收方向：NetEq（抖动缓冲 + 解码 + PLC）-> ADM 播放
//
// 本项目把 APM 简化为直通（3A 是独立课题，见 docs 说明），
// 其余各层均由本管线编排。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "media/jitter_buffer.h"
#include "media/opus_codec.h"
#include "media/virtual_device.h"
#include "protocol/packet.h"

namespace rtc {

class AudioPipeline {
public:
    // 编码后产出一个媒体包的出口（由上层交给传输层发送）
    using SendFn = std::function<void(const PacketHeader& h, const std::vector<uint8_t>& payload)>;

    struct Config {
        int  session_id   = 1;
        int  bitrate_bps  = 16000;
        int  frame_ms     = 20;
        int  aggregate_frames = 3;   // 每包聚合帧数（3 × 20ms = 60ms）
        bool use_inband_fec   = false;
        int  jb_target_ms = 60;
    };

    AudioPipeline() = default;
    ~AudioPipeline();

    bool init(const Config& cfg, VirtualAudioDevice* device);
    void set_send_fn(SendFn fn);

    // ---- 发送方向 ----
    void start_sending();
    void stop_sending();

    // ---- 接收方向 ----
    // 收到远端媒体包时调用（已解析出 header 和 payload）
    void on_media_packet(const PacketHeader& h, const std::vector<uint8_t>& payload);

    // 启动接收侧播放（按播放时钟从抖动缓冲取帧、解码、送到设备）
    void start_playout();
    void stop_playout();

    // ---- 打断语义（对应简历中的 trans_id 过滤）----
    // 标记某事务被打断：之后同 trans_id 的在途数据全部丢弃
    void interrupt(uint16_t trans_id);
    void clear_interrupt();
    bool is_interrupted(uint16_t trans_id) const;

    // ---- 统计 ----
    struct Report {
        uint64_t sent_packets = 0;
        uint64_t sent_bytes = 0;
        uint64_t recv_packets = 0;
        uint64_t recv_bytes = 0;
        uint64_t interrupted_drops = 0;
        uint64_t encode_errors = 0;
        uint64_t decode_errors = 0;
        double   cur_bitrate_kbps = 0;
        JitterBuffer::Stats jb;
    };
    Report report() const;

    int  bitrate_kbps() const;
    void set_target_bitrate_bps(int bps);   // 供拥塞控制调用

private:
    void send_loop();
    void playout_loop();

    // 不加锁的内部版本。
    // 存在的原因：std::mutex 不是递归锁，公共方法之间不能互相调用
    // （report() 里调 bitrate_kbps() 会导致同一线程重复加锁 → 自死锁）。
    // 所以把"需要锁保护的实现"和"负责加锁的接口"分开。见 docs/02-pitfalls.md 坑 12。
    int bitrate_kbps_locked() const;

    Config cfg_{};
    VirtualAudioDevice* device_ = nullptr;
    OpusCodec codec_;
    JitterBuffer jb_;
    SendFn send_fn_;

    std::atomic<bool> sending_{false};
    std::atomic<bool> playout_{false};
    std::thread send_thread_;
    std::thread playout_thread_;

    mutable std::mutex stat_mutex_;
    Report report_{};

    // 发送侧：攒帧用于聚合
    // 注意：这两个必须是成员（而非 send_loop 的局部变量），
    // 因为 stop_sending() 需要能唤醒阻塞在 wait 上的发送线程——
    // 否则一旦"采集已停但帧数没攒够"，线程会永远等不到通知。见 docs/02-pitfalls.md 坑 14。
    std::mutex              send_mutex_;
    std::condition_variable send_cv_;
    std::vector<std::vector<uint8_t>> pending_frames_;
    uint16_t seq_ = 0;
    uint32_t timestamp_ = 0;

    // 打断状态
    std::atomic<uint16_t> interrupted_trans_id_{0};
    std::atomic<bool>     has_interrupt_{false};
    std::atomic<uint16_t> cur_trans_id_{1};
};

}  // namespace rtc
