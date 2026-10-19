// 通话端点（客户端）
//
// 本阶段提供可运行骨架：参数解析 + 组件初始化 + 本地音频自环验证。
// 后续阶段将接入：WebSocket 信令连接、libnice ICE、DTLS-SRTP 加密。
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/util.h"
#include "media/audio_pipeline.h"
#include "media/opus_codec.h"
#include "media/virtual_device.h"
#include "protocol/packet.h"
#include "protocol/signaling_msg.h"

using namespace rtc;

namespace {

struct Args {
    std::string signal_url = "ws://127.0.0.1:8888";
    std::string room = "demo";
    std::string audio_file;                 // 采集源 PCM
    std::string out_file = "/tmp/peer_out.pcm";
    int         seconds = 5;
    int         bitrate = 16000;
    int         jb_target = 60;
    bool        loopback = false;           // 本地自环：不经过网络，验证媒体链路
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        if (k == "--signal" && i + 1 < argc) a.signal_url = argv[++i];
        else if (k == "--room" && i + 1 < argc) a.room = argv[++i];
        else if (k == "--audio-file" && i + 1 < argc) a.audio_file = argv[++i];
        else if (k == "--out" && i + 1 < argc) a.out_file = argv[++i];
        else if (k == "--seconds" && i + 1 < argc) a.seconds = atoi(argv[++i]);
        else if (k == "--bitrate" && i + 1 < argc) a.bitrate = atoi(argv[++i]);
        else if (k == "--jb" && i + 1 < argc) a.jb_target = atoi(argv[++i]);
        else if (k == "--loopback") a.loopback = true;
        else if (k == "--verbose") set_log_level(LogLevel::kDebug);
    }
    return a;
}

void usage() {
    printf("用法: peer [选项]\n");
    printf("  --signal <url>     信令服务器地址 (默认 ws://127.0.0.1:8888)\n");
    printf("  --room <name>      房间名 (默认 demo)\n");
    printf("  --audio-file <pcm> 采集源 PCM 文件（默认正弦波合成）\n");
    printf("  --out <pcm>        播放落盘文件 (默认 /tmp/peer_out.pcm)\n");
    printf("  --seconds <n>      运行时长 (默认 5)\n");
    printf("  --bitrate <bps>    Opus 码率 (默认 16000)\n");
    printf("  --jb <ms>          抖动缓冲目标延迟 (默认 60)\n");
    printf("  --loopback         本地自环验证（不经网络）\n");
    printf("  --verbose          打开调试日志\n");
}

// 自环验证：编码 -> 包序列化 -> 反序列化 -> 抖动缓冲 -> 解码 -> 播放
// 这条链路把"本地媒体面"完整跑通，不依赖网络。
// 意义：网络部分出问题时，可以先用 loopback 确认媒体链路本身是正确的。
int run_loopback(const Args& args) {
    printf("=== 本地自环验证（loopback）===\n");

    VirtualAudioDevice::Config dcfg;
    dcfg.source_path = args.audio_file;
    dcfg.sink_path = args.out_file;
    VirtualAudioDevice device(dcfg);
    if (!device.open()) {
        fprintf(stderr, "打开虚拟设备失败\n");
        return 1;
    }

    AudioPipeline::Config pcfg;
    pcfg.session_id = 1;
    pcfg.bitrate_bps = args.bitrate;
    pcfg.jb_target_ms = args.jb_target;
    pcfg.aggregate_frames = 3;

    AudioPipeline pipe;
    if (!pipe.init(pcfg, &device)) {
        fprintf(stderr, "管线初始化失败\n");
        return 1;
    }

    // 发送出口 = 直接回灌进接收侧（模拟"网络零延迟、零丢包"）
    std::atomic<int> looped{0};
    pipe.set_send_fn([&](const PacketHeader& h, const std::vector<uint8_t>& payload) {
        // 真实项目这里会走传输层；自环时直接序列化再解析，
        // 顺便验证协议编解码的正确性
        uint8_t buf[4096];
        int n = Packet::serialize(h, payload.data(), payload.size(), buf, sizeof(buf));
        if (n > 0) {
            PacketHeader ph;
            std::vector<uint8_t> pp;
            if (Packet::parse(buf, size_t(n), &ph, &pp)) {
                pipe.on_media_packet(ph, pp);
                looped++;
            }
        }
    });

    pipe.start_playout();
    device.start_playout();   // 启动"播放消费"线程，模拟 DMA 取走数据
    pipe.start_sending();

    std::this_thread::sleep_for(std::chrono::seconds(args.seconds));

    pipe.stop_playout();      // 先停播放管线（不再写数据）
    device.stop_playout();    // 再停设备消费线程
    pipe.stop_sending();

    auto rep = pipe.report();
    printf("\n--- 自环结果（%d 秒）---\n", args.seconds);
    printf("  发送包数      : %llu\n", (unsigned long long)rep.sent_packets);
    printf("  自环回灌包数  : %d\n", looped.load());
    printf("  接收包数      : %llu\n", (unsigned long long)rep.recv_packets);
    printf("  编码错误      : %llu\n", (unsigned long long)rep.encode_errors);
    printf("  解码错误      : %llu\n", (unsigned long long)rep.decode_errors);
    printf("  抖动缓冲接收  : %llu\n", (unsigned long long)rep.jb.received);
    printf("  播放帧数      : %llu\n", (unsigned long long)rep.jb.played);
    printf("  PLC 补偿      : %llu\n", (unsigned long long)rep.jb.plc);
    printf("  目标延迟      : %.0f ms\n", rep.jb.cur_target_ms);
    printf("  实测码率      : %.1f kbps\n", rep.cur_bitrate_kbps);
    printf("  播放落盘      : %s (%.1f s)\n", args.out_file.c_str(),
           double(device.play_stats().played_samples) / 16000.0);

    const bool ok = rep.sent_packets > 0 && rep.jb.played > 0 && rep.decode_errors == 0;
    printf("\n%s\n", ok ? "✅ 自环验证通过：媒体链路端到端可用"
                       : "❌ 自环验证失败：请检查日志");
    device.close();
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse(argc, argv);

    if (argc <= 1) {
        usage();
        printf("\n提示：先用 --loopback 验证本地媒体链路，再接入网络。\n");
        return 0;
    }

    printf("=== RTC 端點 (peer) ===\n");
    printf("  信令 : %s\n", args.signal_url.c_str());
    printf("  房间 : %s\n", args.room.c_str());

    if (args.loopback) {
        return run_loopback(args);
    }

    printf("\n⚠️  网络模式尚未接入（后续阶段实现）：\n");
    printf("   - 阶段 4：WebSocket 信令连接\n");
    printf("   - 阶段 5：libnice ICE / STUN 穿透\n");
    printf("   - 阶段 6：DTLS-SRTP 加密\n");
    printf("  当前可用：peer --loopback  （本地媒体链路验证）\n");
    return 0;
}
