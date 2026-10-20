#pragma once
// 虚拟视频设备 —— 对应 WebRTC 的 VideoCaptureModule / VideoRenderModule
//
// 与 VirtualAudioDevice 的对称设计：
//   采集：按 fps 定时产出 YUV420 帧（测试图案 / 文件）
//   播放：接收解码后的 YUV 帧，统计并落盘（PPM 序列，可用图片查看器打开）
//
// 为什么需要"虚拟"：
//   WSL 没有摄像头（/dev/video*），也没有显示设备。
//   但视频链路（编码/打包/抖动缓冲/关键帧策略/解码）必须能完整验证。
//   虚拟设备提供与真实设备一致的接口语义，换成真实摄像头时上层不用改。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/vp8_codec.h"   // YuvFrame

namespace rtc {

class VirtualVideoDevice {
public:
    struct Config {
        int         width  = 320;
        int         height = 240;
        int         fps    = 15;
        // 采集源：
        //   "test:bars"   彩条（标准测试图）
        //   "test:motion" 移动方块 + 渐变（最推荐：既有细节又有运动，能测出码率）
        //   "test:noise"  噪声（最难压缩，压力测试）
        std::string source = "test:motion";
        // 播放落盘目录（空则不落盘）。会写出 out_0000.ppm, out_0001.ppm ...
        std::string sink_dir;
        int         max_sink_frames = 0;   // 0 = 不限
    };

    explicit VirtualVideoDevice(const Config& cfg);
    ~VirtualVideoDevice();

    bool open();
    void close();

    // ---- 采集 ----
    // 取下一帧（按 fps 节拍阻塞等待）。返回 false 表示已停止。
    bool capture(YuvFrame* out);
    void start_capture();
    void stop_capture();

    // ---- 播放 ----
    void play(const YuvFrame& frame);
    void start_playout();
    void stop_playout();

    struct Stats {
        uint64_t captured_frames = 0;
        uint64_t played_frames   = 0;
        uint64_t sink_frames     = 0;
        double   capture_fps     = 0;
        double   playout_fps     = 0;
        size_t   play_queue_depth = 0;
    };
    Stats stats() const;

    const Config& config() const { return cfg_; }

private:
    void capture_loop();
    void playout_loop();
    void generate_frame(YuvFrame* f, int64_t frame_no);
    bool write_ppm(const YuvFrame& f, const std::string& path);

    Config cfg_;
    std::atomic<bool> capture_running_{false};
    std::atomic<bool> playout_running_{false};
    std::atomic<bool> opened_{false};
    std::thread capture_thread_;
    std::thread playout_thread_;

    // 采集侧：节拍控制
    mutable std::mutex cap_mutex_;
    std::condition_variable cap_cv_;
    std::deque<YuvFrame> cap_queue_;
    int64_t cap_frame_no_ = 0;
    int64_t cap_start_ms_ = 0;

    // 播放侧
    mutable std::mutex play_mutex_;
    std::deque<YuvFrame> play_queue_;
    mutable Stats stats_;
};

}  // namespace rtc
