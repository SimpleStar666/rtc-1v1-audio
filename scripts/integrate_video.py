#!/usr/bin/env python3
# 把视频接入 call_session（复用同一条 ICE+SRTP 通道，session_id=2）
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/app/call_session.cpp'
s = open(P, encoding='utf-8').read()
n = 0
def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# ---- 1) include ----
rep('#include "media/virtual_device.h"',
    '#include "media/video_device.h"\n#include "media/video_pipeline.h"\n#include "media/virtual_device.h"',
    'include 视频头文件')

# ---- 2) 接收路径：按 session_id 分流到音频/视频 ----
rep("""        PacketHeader ph;
        std::vector<uint8_t> payload;
        if (!Packet::parse(buf.data(), size_t(n), &ph, &payload)) return;
        // 只处理媒体流（session_id=1），FEC 包走别的 SSRC
        if (ph.session_id != 1) return;
        media_rx++;
        pipe.on_media_packet(ph, payload);
    });""",
"""        PacketHeader ph;
        std::vector<uint8_t> payload;
        if (!Packet::parse(buf.data(), size_t(n), &ph, &payload)) return;

        // ★ 按 SSRC（session_id）分流 —— 这是多流复用的核心
        //   1 = 音频（Opus）
        //   2 = 视频（VP8，独立序号空间）
        //   其他 = FEC 等辅助流
        // 混流会导致序号空间互相干扰（坑 30/34 的教训）。
        if (ph.session_id == 1) {
            media_rx++;
            pipe.on_media_packet(ph, payload);
        } else if (ph.session_id == 2) {
            video_rx++;
            vpipe.on_video_fragment(ph, payload);
        }
    });""",
'接收路径分流音频/视频')

# ---- 3) 声明视频相关变量（放在 AudioPipeline 之前）----
rep("""    AudioPipeline pipe;
    // 接收路径：ICE -> 分流 -> SRTP 解密 -> 解析包头 -> 喂给管线""",
"""    AudioPipeline pipe;

    // ---- 视频：虚拟摄像头 + 视频管线（session_id=2，与音频共用同一条 ICE+SRTP 通道）----
    VirtualVideoDevice::Config vdcfg;
    vdcfg.width  = opt.video_width;
    vdcfg.height = opt.video_height;
    vdcfg.fps    = opt.video_fps;
    vdcfg.source = opt.video_source;
    vdcfg.sink_dir = opt.video_sink_dir;   // 非空则把解码后的帧落盘为 PPM
    VirtualVideoDevice vdevice(vdcfg);
    if (opt.video && !vdevice.open()) {
        fprintf(stderr, "  ✗ 虚拟视频设备打开失败（视频功能关闭）\\n");
        opt_video_disabled = true;
    }
    VideoPipeline vpipe;
    std::atomic<int> video_rx{0};

    // 接收路径：ICE -> 分流 -> SRTP 解密 -> 解析包头 -> 喂给管线""",
'声明视频设备与管线')

# ---- 4) 初始化视频管线 + 接线（放在音频 init 之后、start 之前）----
rep("""    pipe.start_playout();     // 启动播放（抖动缓冲 -> 解码 -> 写设备）
    device.start_playout();   // 启动设备消费线程（模拟 DMA 取走数据）
    pipe.start_sending();     // 启动采集+编码+发送""",
"""    // ---- 视频管线初始化与接线 ----
    const bool video_on = opt.video && !opt_video_disabled;
    if (video_on) {
        VideoPipeline::Config vcfg;
        vcfg.session_id   = 2;                     // 视频独立 SSRC
        vcfg.width        = opt.video_width;
        vcfg.height       = opt.video_height;
        vcfg.fps          = opt.video_fps;
        vcfg.bitrate_kbps = opt.video_bitrate_kbps;
        vcfg.keyframe_interval = opt.video_fps * 2;   // 每 2 秒一个关键帧
        vcfg.sink_frames  = !opt.video_sink_dir.empty();
        vcfg.sink_dir     = opt.video_sink_dir;

        if (!vpipe.init(vcfg, &vdevice)) {
            fprintf(stderr, "  ✗ 视频管线初始化失败（仅音频继续）\\n");
        } else {
            // 发送：视频分片 -> 序列化 -> SRTP 加密 -> ICE（与音频同一条通道）
            vpipe.set_send_fn([&](const PacketHeader& h,
                                  const std::vector<uint8_t>& payload) {
                uint8_t buf[2048];
                const int n = Packet::serialize(h, payload.data(), payload.size(),
                                                buf, sizeof(buf));
                if (n <= 0) return;
                int len = n;
                if (!dtls.protect(buf, &len)) return;
                ice.send(buf, size_t(len));
                video_tx++;
            });
            // 关键帧请求（对应 RTCP PLI）：走信令通道，可靠且低频
            vpipe.set_keyframe_request_fn([&] {
                SignalMsg m;
                m.type = SignalType::kKeyframeRequest;
                m.room = opt.room;
                sig.send(m);
                kf_req_sent++;
            });

            vdevice.start_capture();
            vdevice.start_playout();
            vpipe.start();
            printf("  视频: %dx%d @%dfps, %dkbps, session_id=2\\n",
                   opt.video_width, opt.video_height, opt.video_fps,
                   opt.video_bitrate_kbps);
        }
    }

    pipe.start_playout();     // 启动播放（抖动缓冲 -> 解码 -> 写设备）
    device.start_playout();   // 启动设备消费线程（模拟 DMA 取走数据）
    pipe.start_sending();     // 启动采集+编码+发送""",
'视频管线初始化与接线')

# ---- 5) 处理对端的关键帧请求（信令回调里）----
rep("""            case SignalType::kPeerLeft:
                peer_left = true;
                printf("  [信令] 对端离开\\n");
                break;""",
"""            case SignalType::kKeyframeRequest:
                // 对端丢帧了，要求我们下一帧发关键帧（对应 RTCP PLI）
                printf("  [信令] 对端请求关键帧（PLI）\\n");
                kf_req_recv++;
                if (video_on_ptr && video_on_ptr->load()) {
                    // 在锁外处理，避免持信令锁调用编码器
                    need_kf.store(true);
                }
                break;
            case SignalType::kPeerLeft:
                peer_left = true;
                printf("  [信令] 对端离开\\n");
                break;""",
'处理对端关键帧请求')

# ---- 6) 收尾：停止视频 ----
rep("""    // 收尾：先停管线再停设备
    pipe.stop_playout();
    device.stop_playout();
    pipe.stop_sending();""",
"""    // 收尾：先停管线再停设备（视频同理）
    if (video_on) {
        vpipe.stop();
        vdevice.stop_capture();
        vdevice.stop_playout();
    }
    pipe.stop_playout();
    device.stop_playout();
    pipe.stop_sending();""",
'收尾停止视频')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ call_session 共 {n} 处改动')
