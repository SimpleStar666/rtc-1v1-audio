// 通话端点（客户端）
//
// 本阶段提供可运行骨架：参数解析 + 组件初始化 + 本地音频自环验证。
// 后续阶段将接入：WebSocket 信令连接、libnice ICE、DTLS-SRTP 加密。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/call_session.h"
#include "common/logging.h"
#include "common/util.h"
#include "media/audio_pipeline.h"
#include "media/opus_codec.h"
#include "media/virtual_device.h"
#include "protocol/packet.h"
#include "protocol/signaling_msg.h"
#include "signal/signaling_client.h"

using namespace rtc;

namespace {

struct Args {
    std::string signal_url = "ws://127.0.0.1:8888";
    std::string room = "demo";
    std::string name = "peer";
    std::string audio_file;                 // 采集源 PCM
    std::string out_file = "/tmp/peer_out.pcm";
    int         seconds = 5;
    int         bitrate = 16000;
    int         jb_target = 60;
    bool        loopback = false;           // 本地自环：不经过网络
    bool        peer_test = false;          // 仅信令配对（诊断用）
    bool        call = false;               // 完整通话（真实媒体传输）
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        if (k == "--signal" && i + 1 < argc) a.signal_url = argv[++i];
        else if (k == "--room" && i + 1 < argc) a.room = argv[++i];
        else if (k == "--name" && i + 1 < argc) a.name = argv[++i];
        else if (k == "--audio-file" && i + 1 < argc) a.audio_file = argv[++i];
        else if (k == "--out" && i + 1 < argc) a.out_file = argv[++i];
        else if (k == "--seconds" && i + 1 < argc) a.seconds = atoi(argv[++i]);
        else if (k == "--bitrate" && i + 1 < argc) a.bitrate = atoi(argv[++i]);
        else if (k == "--jb" && i + 1 < argc) a.jb_target = atoi(argv[++i]);
        else if (k == "--loopback") a.loopback = true;
        else if (k == "--peer-test") a.peer_test = true;
        else if (k == "--call") a.call = true;
        else if (k == "--verbose") set_log_level(LogLevel::kDebug);
    }
    return a;
}

void usage() {
    printf("用法: peer [选项]\n");
    printf("  --signal <url>     信令服务器地址 (默认 ws://127.0.0.1:8888)\n");
    printf("  --room <name>      房间名 (默认 demo)\n");
    printf("  --name <id>        本端标识 (默认 peer)\n");
    printf("  --audio-file <pcm> 采集源 PCM 文件（默认正弦波合成）\n");
    printf("  --out <pcm>        播放落盘文件 (默认 /tmp/peer_out.pcm)\n");
    printf("  --seconds <n>      运行时长 (默认 5)\n");
    printf("  --bitrate <bps>    Opus 码率 (默认 16000)\n");
    printf("  --jb <ms>          抖动缓冲目标延迟 (默认 60)\n");
    printf("\n模式（三选一）:\n");
    printf("  --call             完整 1v1 通话：信令 + ICE + DTLS-SRTP + 双向音频\n");
    printf("                     （两端真的互相传音频，这是最主要的用法）\n");
    printf("  --loopback         本地自环验证（不经网络，验证媒体链路本身）\n");
    printf("  --peer-test        仅信令配对（诊断用，不含真实媒体传输）\n");
    printf("\n其他:\n");
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

// 双端信令配对测试
//
// 流程（模拟 WebRTC 建链的信令部分）：
//   1. 两端各自连接信令服务器，发送 join 加入同一房间
//   2. 服务器配对成功后，向双方推送 peer_ready
//   3. 先入房方发 offer，后入房方回 answer（信令服务器转发）
//   4. 双方交换一个 candidate（第一阶段用真实的 UDP 端口占位）
//   5. 双方各自用 loopback 跑媒体，验证"信令通了、媒体也通"
//
// 本阶段的重点：把信令面的完整交互跑通。
// 真正的媒体要在后续阶段经 libnice 打通候选地址后传输。
int run_peer_test(const Args& args) {
    printf("\n=== 双端信令配对测试 ===\n");

    SignalingClient sig;
    std::atomic<bool> joined{false};
    std::atomic<bool> peer_ready{false};
    std::atomic<bool> got_offer{false};
    std::atomic<bool> got_answer{false};
    std::atomic<bool> got_candidate{false};
    std::atomic<bool> media_should_start{false};

    // 会话参数（本项目用 JSON 传简化版"会话描述"）
    const std::string sdp =
        "codec=opus;rate=16000;ch=1;frame=20;ptime=60";

    sig.set_on_message([&](const SignalMsg& m) {
        switch (m.type) {
            case SignalType::kJoined:
                joined = true;
                printf("  [%s] 已入房（成员数 %s）\n", args.name.c_str(),
                       m.sdp.empty() ? "?" : m.sdp.c_str());
                break;

            case SignalType::kPeerReady:
                peer_ready = true;
                printf("  [%s] 对端就绪 -> 可以开始协商\n", args.name.c_str());
                break;

            case SignalType::kOffer: {
                got_offer = true;
                printf("  [%s] 收到 offer: %s\n", args.name.c_str(), m.sdp.c_str());
                // 回 answer
                SignalMsg ans = SignalMsg::make_answer(args.room, sdp);
                sig.send(ans);
                printf("  [%s] 已回 answer\n", args.name.c_str());
                // 发候选（第一阶段用占位地址，后续接 libnice）
                SignalMsg cand = SignalMsg::make_candidate(args.room, "127.0.0.1:50000", 0);
                sig.send(cand);
                media_should_start = true;
                break;
            }

            case SignalType::kAnswer:
                got_answer = true;
                printf("  [%s] 收到 answer: %s\n", args.name.c_str(), m.sdp.c_str());
                // 发候选
                {
                    SignalMsg cand = SignalMsg::make_candidate(args.room, "127.0.0.1:50001", 0);
                    sig.send(cand);
                }
                media_should_start = true;
                break;

            case SignalType::kCandidate:
                got_candidate = true;
                printf("  [%s] 收到 candidate: %s (mline=%d)\n", args.name.c_str(),
                       m.candidate.c_str(), m.mline_index);
                break;

            case SignalType::kPeerLeft:
                printf("  [%s] 对端离开（%s）\n", args.name.c_str(), m.reason.c_str());
                break;

            case SignalType::kError:
                printf("  [%s] 服务器错误: %s\n", args.name.c_str(), m.reason.c_str());
                break;

            default:
                break;
        }
    });

    sig.set_on_state([&](bool connected) {
        printf("  [%s] 信令连接%s\n", args.name.c_str(), connected ? "建立" : "断开");
        if (connected) {
            sig.send(SignalMsg::make_join(args.room));
        }
    });

    if (!sig.connect(args.signal_url)) {
        fprintf(stderr, "连接信令服务器失败\n");
        return 1;
    }

    if (!sig.wait_connected(5000)) {
        fprintf(stderr, "连接信令服务器超时（服务器未启动？）\n");
        sig.disconnect();
        return 1;
    }

    // 等入房。
    // 注意：`joined` 与 `peer_ready` 可能几乎同时到达（服务器成对推送），
    // 也可能因为消息顺序而只收到其一。这里用"任一到齐即视为配对完成"的判断，
    // 避免因消息竞态误判超时。见 docs/02-pitfalls.md 坑 19。
    for (int i = 0; i < 50 && !(joined && peer_ready); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!joined && !peer_ready) {
        fprintf(stderr, "入房/配对超时\n");
        sig.disconnect();
        return 1;
    }
    if (!joined) {
        // 服务器可能在 peer_ready 之后才推 joined（顺序不定），补等一小会儿
        for (int i = 0; i < 10 && !joined; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    if (peer_ready && !got_offer && !got_answer) {
        // 用名称决定谁先发（简单且确定的规则）
        // 名字小的先发 offer
        if (args.name == "A" || args.name < "B") {
            printf("  [%s] 我发 offer\n", args.name.c_str());
            sig.send(SignalMsg::make_offer(args.room, sdp));
        } else {
            printf("  [%s] 等对方发 offer\n", args.name.c_str());
        }
    }

    // 等协商完成
    for (int i = 0; i < 50 && !media_should_start; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    printf("\n--- 信令交互结果 [%s] ---\n", args.name.c_str());
    printf("  入房成功    : %s\n", joined ? "是" : "否");
    printf("  对端就绪    : %s\n", peer_ready ? "是" : "否");
    printf("  收到 offer  : %s\n", got_offer ? "是" : "否");
    printf("  收到 answer : %s\n", got_answer ? "是" : "否");
    printf("  收到候选    : %s\n", got_candidate ? "是" : "否");
    printf("  收/发消息数 : %llu / %llu\n",
           (unsigned long long)sig.rx_count(), (unsigned long long)sig.tx_count());

    // 判定"信令面通过"：入房 + 配对 + 候选交换成功
    // 注意是 (offer 或 answer) 之一成立即可——主叫收 answer、被叫收 offer，
    // 两端不可能同时收到两者。见 docs/02-pitfalls.md 坑 19。
    const bool sig_ok = joined && peer_ready && got_candidate &&
                        (got_offer || got_answer);
    printf("  信令面结果  : %s%s\n", sig_ok ? "✅ 通过" : "❌ 未完成",
           (got_answer && !got_offer) ? "（本端为主叫，收到 answer）"
                                      : (got_offer ? "（本端为被叫，收到 offer）" : ""));

    // 信令通了之后，跑媒体（本阶段仍用 loopback，因为还没接 ICE）
    if (sig_ok) {
        printf("\n--- 媒体面验证（loopback）---\n");
        Args lo = args;
        lo.loopback = true;
        lo.seconds = args.seconds;
        run_loopback(lo);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sig.disconnect();
    return sig_ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse(argc, argv);

    if (argc <= 1) {
        usage();
        printf("\n提示：\n");
        printf("  --call       真正的 1v1 通话（信令 + ICE + DTLS-SRTP + 双向音频）\n");
        printf("  --loopback   只验证本地媒体链路（不经网络）\n");
        printf("  --peer-test  只验证信令配对（诊断用）\n");
        return 0;
    }

    printf("=== RTC 端點 (peer) ===\n");
    printf("  名称 : %s\n", args.name.c_str());
    printf("  信令 : %s\n", args.signal_url.c_str());
    printf("  房间 : %s\n", args.room.c_str());

    // 优先级：--call（真实通话）> --loopback（本地自环）> --peer-test（仅信令）
    if (args.call) {
        CallOptions copt;
        copt.signal_url = args.signal_url;
        copt.room       = args.room;
        copt.name       = args.name;
        copt.seconds    = args.seconds;
        copt.bitrate    = args.bitrate;
        copt.jb_ms      = args.jb_target;
        copt.audio_file = args.audio_file;
        copt.out_file   = args.out_file;
        return run_call(copt);
    }
    if (args.loopback) {
        return run_loopback(args);
    }
    if (args.peer_test) {
        return run_peer_test(args);
    }

    usage();
    return 0;
}
