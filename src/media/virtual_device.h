#pragma once
// 虚拟音频设备 —— 对应 WebRTC 的 ADM (Audio Device Module)
//
// 为什么需要虚拟设备：
//   本机 WSL 无 /dev/snd 声卡（只有 timer），无法打开真实麦克风/扬声器。
//   因此实现一个"文件/合成音频"驱动的设备，提供与真实设备一致的接口语义：
//     - 采集：按采样率定时产出 PCM（可由文件回放或正弦波合成）
//     - 播放：接收 PCM，按播放时钟消费（可写入文件供离线分析）
//
// 这个设计本身就是 ADM 的核心价值：把"音频从哪来、到哪去"与业务逻辑解耦。
// 换成真实设备时，只需替换本类的实现，上层管线不变。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rtc {

class VirtualAudioDevice {
public:
    // 采集回调：按 10ms 粒度产出一帧 PCM（160 样本 @16kHz）
    using CaptureCallback = std::function<void(const int16_t* pcm, int samples, int64_t ts_ms)>;

    struct Config {
        int         sample_rate = 16000;
        int         channels    = 1;
        int         frame_ms    = 20;     // 采集粒度
        std::string source_path;          // 采集源（PCM 文件）；为空则用正弦波合成
        std::string sink_path;            // 播放落地文件；为空则不落盘
    };

    explicit VirtualAudioDevice(const Config& cfg);
    ~VirtualAudioDevice();

    // 打开设备
    bool open();
    void close();

    // 启动/停止采集线程
    void start_capture(CaptureCallback cb);
    void stop_capture();

    // 启动/停止"播放消费"线程。
    // 真实声卡由 DMA 按采样率把缓冲里的数据取走；虚拟设备必须自己模拟这个过程，
    // 否则只写不读，缓冲必然填满并开始丢数据（早期版本就是这个问题，
    // 日志里刷屏 "play buffer full"）。见 docs/02-pitfalls.md 坑 13。
    void start_playout();
    void stop_playout();

    // 播放：把 PCM 追加到播放缓冲（模拟"写入扬声器"）
    bool play(const int16_t* pcm, int samples);

    // 播放侧统计：实际消费的样本数、缓冲水位
    struct PlayStats {
        uint64_t played_samples = 0;   // 被"设备"消费掉的样本数
        size_t   buffer_level   = 0;
        double   fill_level     = 0;   // 0~1
        uint64_t underruns      = 0;   // 缓冲空但设备要数据的次数
    };
    PlayStats play_stats() const;
    void      set_play_buffer_capacity(size_t samples);

    // 采集侧：还剩多少音频可采（文件回放时用于判断结束）
    bool     capture_exhausted() const;
    uint64_t captured_samples() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rtc
