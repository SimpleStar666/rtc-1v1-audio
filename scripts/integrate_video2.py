#!/usr/bin/env python3
# 把视频接入 call_session（一次性、完整、变量作用域正确）
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/app/call_session.cpp'
s = open(P, encoding='utf-8').read()
n = 0
def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return False
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1
    return True

# ============ 1) include ============
rep('#include "media/virtual_device.h"',
    '#include "media/video_device.h"\n#include "media/video_pipeline.h"\n#include "media/virtual_device.h"',
    'include 视频头文件')

# ============ 2) 在 run_call 开头声明所有视频相关变量（供信令回调捕获）============
rep("""    IceTransport* ice_ptr = nullptr;             // 供消息回调直接应用候选
""",
"""    IceTransport* ice_ptr = nullptr;             // 供消息回调直接应用候选

    // ---- 视频相关（提前声明，因为信令回调要捕获它们）----
    // 视频开关由 opt 决定；如果设备打开失败会自动降级为纯音频
    std::atomic<bool> video_active{false};
    std::atomic<bool> need_keyframe{false};      // 对端请求关键帧（PLI）
    std::atomic<int>  video_tx{0};
    std::atomic<int>  video_rx{0};
    std::atomic<int>  kf_req_sent{0};
    std::atomic<int>  kf_req_recv{0};
""",
'声明视频控制变量')

# ============ 3) 信令回调：处理关键帧请求 ============
rep("""            case SignalType::kPeerLeft:
                peer_left = true;
                printf("  [信令] 对端离开\\n");
                break;""",
"""            case SignalType::kKeyframeRequest:
                // 对端丢帧了，要求我们下一帧发关键帧（对应 RTCP PLI）
                kf_req_recv++;
                need_keyframe.store(true);
                break;
            case SignalType::kPeerLeft:
                peer_left = true;
                printf("  [信令] 对端离开\\n");
                break;""",
'信令回调处理关键帧请求')

# ============ 4) 接收分流：session_id 1=音频 2=视频 ============
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

        // ★ 按 SSRC（session_id）分流 —— 多流复用的核心
        //   1 = 音频（Opus，每包一帧聚合）
        //   2 = 视频（VP8，一帧分成多个分片）
        //   其他 = FEC 等辅助流
        // 混流会让两个序号空间互相干扰（坑 30/34 的教训）。
        if (ph.session_id == 1) {
            media_rx++;
            pipe.on_media_packet(ph, payload);
        } else if (ph.session_id == 2 && video_active.load()) {
            video_rx++;
            vpipe.on_video_fragment(ph, payload);
        }
    });""",
'接收路径按 SSRC 分流')

# ============ 5) 在 AudioPipeline pipe; 之后声明视频设备/管线 ============
rep("""    AudioPipeline pipe;
    // 接收路径：ICE -> 分流 -> SRTP 解密 -> 解析包头 -> 喂给管线""",
"""    AudioPipeline pipe;

    // ---- 视频设备与管线（session_id=2，与音频共用同一条 ICE+SRTP 通道）----
    VirtualVideoDevice::Config vdcfg;
    vdcfg.width    = opt.video_width;
    vdcfg.height   = opt.video_height;
    vdcfg.fps      = opt.video_fps;
    vdcfg.source   = opt.video_source;
    vdcfg.sink_dir = opt.video_sink_dir;   // 非空则把解码帧落盘为 PPM
    VirtualVideoDevice vdevice(vdcfg);
    VideoPipeline vpipe;
    bool video_ready = false;
    if (opt.video) {
        video_ready = vdevice.open();
        if (!video_ready) {
            fprintf(stderr, "  ! 虚拟视频设备打开失败，降级为纯音频\\n");
        }
    }

    // 接收路径：ICE -> 分流 -> SRTP 解密 -> 解析包头 -> 喂给管线""",
'声明视频设备与管线')

# ============ 6) 视频管线初始化 + 接线（在音频 start 之前）============
rep("""    pipe.start_playout();     // 启动播放（抖动缓冲 -> 解码 -> 写设备）
    device.start_playout();   // 启动设备消费线程（模拟 DMA 取走数据）
    pipe.start_sending();     // 启动采集+编码+发送""",
"""    // ---- 视频管线初始化与接线 ----
    if (video_ready) {
        VideoPipeline::Config vcfg;
        vcfg.session_id        = 2;                    // 视频独立 SSRC
        vcfg.width             = opt.video_width;
        vcfg.height            = opt.video_height;
        vcfg.fps               = opt.video_fps;
        vcfg.bitrate_kbps      = opt.video_bitrate_kbps;
        vcfg.keyframe_interval = opt.video_fps * 2;    // 每 2 秒一个关键帧
        vcfg.sink_frames       = !opt.video_sink_dir.empty();
        vcfg.sink_dir          = opt.video_sink_dir;

        if (!vpipe.init(vcfg, &vdevice)) {
            fprintf(stderr, "  ! 视频管线初始化失败，降级为纯音频\\n");
            video_ready = false;
        } else {
            // 发送：视频分片 -> 序列化 -> SRTP 加密 -> ICE（与音频同一条通道）
            vpipe.set_send_fn([&](const PacketHeader& h,
                                  const std::vector<uint8_t>& payload) {
                uint8_t vbuf[2048];
                const int vn = Packet::serialize(h, payload.data(), payload.size(),
                                                 vbuf, sizeof(vbuf));
                if (vn <= 0) return;
                int vlen = vn;
                if (!dtls.protect(vbuf, &vlen)) return;
                ice.send(vbuf, size_t(vlen));
                video_tx++;
            });
            // 关键帧请求（对应 RTCP PLI）：走信令通道
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
            video_active = true;
            printf("  视频: %dx%d @%dfps, %dkbps, session_id=2, 源=%s\\n",
                   opt.video_width, opt.video_height, opt.video_fps,
                   opt.video_bitrate_kbps, opt.video_source.c_str());
        }
    }

    pipe.start_playout();     // 启动播放（抖动缓冲 -> 解码 -> 写设备）
    device.start_playout();   // 启动设备消费线程（模拟 DMA 取走数据）
    pipe.start_sending();     // 启动采集+编码+发送""",
'视频管线初始化与接线')

# ============ 7) 通话中处理关键帧请求（主循环里）============
rep("""        auto rep = pipe.report();
        printf("    [%2ds] 发送 %llu 包 / 接收 %llu 包 | 播放 %llu 帧 | PLC %llu | %.1f kbps\\n",
               s + 1,
               (unsigned long long)rep.sent_packets,
               (unsigned long long)rep.recv_packets,
               (unsigned long long)rep.jb.played,
               (unsigned long long)rep.jb.plc,
               rep.cur_bitrate_kbps);
        fflush(stdout);""",
"""        // 处理对端的关键帧请求（在通话主循环里做，不占信令回调线程）
        if (need_keyframe.exchange(false) && video_active.load()) {
            vpipe.on_keyframe_request();
        }

        auto rep = pipe.report();
        if (video_active.load()) {
            auto vrep = vpipe.report();
            printf("    [%2ds] 音频 %llu/%llu 包 %.0fkbps | 视频 %llu/%llu 帧 %.0fkbps "
                   "解码%llu 丢弃%llu PLI发%llu收%llu\\n",
                   s + 1,
                   (unsigned long long)rep.sent_packets,
                   (unsigned long long)rep.recv_packets,
                   rep.cur_bitrate_kbps,
                   (unsigned long long)vrep.frames_encoded,
                   (unsigned long long)vrep.frames_recv,
                   vrep.actual_kbps,
                   (unsigned long long)vrep.frames_decoded,
                   (unsigned long long)vrep.frames_dropped,
                   (unsigned long long)kf_req_sent.load(),
                   (unsigned long long)kf_req_recv.load());
        } else {
            printf("    [%2ds] 发送 %llu 包 / 接收 %llu 包 | 播放 %llu 帧 | PLC %llu | %.1f kbps\\n",
                   s + 1,
                   (unsigned long long)rep.sent_packets,
                   (unsigned long long)rep.recv_packets,
                   (unsigned long long)rep.jb.played,
                   (unsigned long long)rep.jb.plc,
                   rep.cur_bitrate_kbps);
        }
        fflush(stdout);""",
'通话主循环处理关键帧请求 + 打印视频统计')

# ============ 8) 收尾停止视频 ============
rep("""    // 收尾：先停管线再停设备
    pipe.stop_playout();
    device.stop_playout();
    pipe.stop_sending();""",
"""    // 收尾：先停管线再停设备（视频同理）
    if (video_active.load()) {
        vpipe.stop();
        vdevice.stop_capture();
        vdevice.stop_playout();
        video_active = false;
    }
    pipe.stop_playout();
    device.stop_playout();
    pipe.stop_sending();""",
'收尾停止视频')

# ============ 9) 结果统计里加视频 ============
rep("""    printf("  解码错误      : %llu\\n", (unsigned long long)rep.decode_errors);""",
"""    printf("  解码错误      : %llu\\n", (unsigned long long)rep.decode_errors);

    // ---- 视频统计 ----
    if (opt.video) {
        const auto vrep = vpipe.report();
        printf("\\n  [视频]\\n");
        printf("  采集/编码帧   : %llu / %llu（关键帧 %llu）\\n",
               (unsigned long long)vrep.frames_captured,
               (unsigned long long)vrep.frames_encoded,
               (unsigned long long)vrep.keyframes_sent);
        printf("  发送分片      : %llu（%llu 字节，实测 %.0f kbps）\\n",
               (unsigned long long)vrep.fragments_sent,
               (unsigned long long)vrep.bytes_sent, vrep.actual_kbps);
        printf("  接收分片/完整帧: %llu / %llu\\n",
               (unsigned long long)vrep.fragments_recv,
               (unsigned long long)vrep.frames_recv);
        printf("  解码/丢弃帧   : %llu / %llu\\n",
               (unsigned long long)vrep.frames_decoded,
               (unsigned long long)vrep.frames_dropped);
        printf("  关键帧请求    : 发出 %llu / 收到 %llu（PLI）\\n",
               (unsigned long long)vrep.keyframe_requests,
               (unsigned long long)kf_req_recv.load());
        printf("  收帧率        : %.1f fps（发送 %.1f fps）\\n",
               vrep.fps_in, vrep.fps_out);
        if (!opt.video_sink_dir.empty()) {
            printf("  解码帧落盘    : %s（%llu 帧 PPM）\\n",
                   opt.video_sink_dir.c_str(),
                   (unsigned long long)vdevice.stats().sink_frames);
        }
    }""",
'结果里加视频统计')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ call_session 共 {n} 处改动')
