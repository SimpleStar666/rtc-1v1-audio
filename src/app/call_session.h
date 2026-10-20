#pragma once
// 真实 1v1 通话会话
//
// 与 --peer-test（只验证信令配对）的区别：
//   本模块跑**完整的媒体链路**，两端真的互相传音频：
//
//     A: 采集 -> Opus 编码 -> 打包(+FEC) -> DTLS-SRTP 加密 -> ICE(UDP) ──►
//     B: ICE 收到 -> SRTP 解密 -> 抖动缓冲 -> Opus 解码 -> 播放(能听到)
//     反向同理（双向通话）
//
// 与 WebRTC 的对应关系：
//   SignalingClient        -> 信令（WebRTC 不规定协议，这里自定义 JSON）
//   IceTransport           -> ICE / STUN / Trickle ICE
//   DtlsSrtpSession        -> DTLS 握手 + SRTP 加密
//   AudioPipeline          -> 采集/编码/聚合/抖动缓冲/播放（ADM + Opus + NetEq）
#include <string>

namespace rtc {

struct CallOptions {
    std::string signal_url = "ws://127.0.0.1:8899";
    std::string room       = "demo";
    std::string name       = "peer";
    int         seconds    = 8;        // 通话时长
    int         bitrate    = 16000;    // Opus 目标码率
    int         jb_ms      = 60;       // 抖动缓冲目标延迟
    std::string audio_file;            // 采集源 PCM（空=正弦波合成）
    std::string out_file   = "/tmp/peer_out.pcm";   // 播放落盘

    // 是否播放声音（WSL 有 WSLg 音频转发，可以真的听到）
    bool        play_audio = true;
};

// 运行一次完整通话。返回 0 表示成功建链并传完音频。
int run_call(const CallOptions& opt);

}  // namespace rtc
