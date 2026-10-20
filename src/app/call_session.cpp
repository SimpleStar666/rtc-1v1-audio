#include "app/call_session.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/util.h"
#include "crypto/dtls_srtp.h"
#include "ice/ice_transport.h"
#include "media/audio_pipeline.h"
#include "media/video_device.h"
#include "media/video_pipeline.h"
#include "media/virtual_device.h"
#include "net/loss_protection.h"
#include "protocol/packet.h"
#include "protocol/signaling_msg.h"
#include "signal/signaling_client.h"

namespace rtc {

namespace {
constexpr const char* kTag = "call";

// 把 ICE 通道包装成 DTLS 的传输层。
//
// 为什么要中间这一层：
//   DTLS 需要的是"发一段字节 / 收一段字节"的抽象，
//   而 ICE 提供的是"候选对选通后收发 UDP 数据报"。
//   这层适配把两者接起来，同时充当 DTLS 记录的入队缓冲。
class IceDtlsTransport : public DtlsTransport {
public:
    explicit IceDtlsTransport(IceTransport* ice) : ice_(ice) {}

    int send(const uint8_t* data, size_t len) override {
        return ice_->send(data, len) ? int(len) : -1;
    }

    int recv(uint8_t* buf, size_t cap, int timeout_ms) override {
        // 必须真正等待 timeout_ms。
        // 若"队列空就立刻返回 0"，DTLS 握手循环会变成忙等，
        // 在极短的真实时间内耗光逻辑超时预算 → 握手必然失败。
        // 见 docs/02-pitfalls.md 坑 29。
        const int64_t deadline = now_ms() + timeout_ms;
        while (now_ms() <= deadline) {
            {
                std::lock_guard<std::mutex> lk(m_);
                if (!q_.empty()) {
                    const size_t n = std::min(cap, q_.size());
                    for (size_t i = 0; i < n; i++) {
                        buf[i] = q_.front();
                        q_.pop_front();
                    }
                    return int(n);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;
    }

    void feed(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < len; i++) q_.push_back(data[i]);
    }

private:
    IceTransport*       ice_;
    std::deque<uint8_t> q_;
    std::mutex          m_;
};

// 判断一个 UDP 负载是 SRTP/RTP 还是 DTLS 记录。
//
// RTP 首字节高 2 位是版本号 = 2 → (b & 0xC0) == 0x80
// DTLS 首字节是内容类型：20=change_cipher_spec, 21=alert, 22=handshake, 23=app_data
// 两者不会冲突，可以可靠分流。
bool looks_like_rtp(const uint8_t* d, size_t n) {
    return n >= 12 && (d[0] & 0xC0) == 0x80;
}

}  // namespace

// ------------------------------------------------------------------
// 通话主流程
// ------------------------------------------------------------------
int run_call(const CallOptions& opt) {
    printf("\n=== 1v1 实时通话（完整媒体链路）===\n");
    printf("  信令 : %s\n", opt.signal_url.c_str());
    printf("  房间 : %s\n", opt.room.c_str());
    printf("  本端 : %s\n", opt.name.c_str());
    printf("  时长 : %d 秒\n\n", opt.seconds);

    // ============================================================
    // 阶段 1：连接信令 + 入房
    // ============================================================
    printf("--- 阶段 1/6：信令连接 ---\n");

    SignalingClient sig;
    std::mutex              mu;
    std::condition_variable cv;

    bool joined = false, peer_ready = false, peer_left = false;
    bool got_offer = false, got_answer = false;
    int  my_member_count = 0;                    // 入房时房间成员数（1=先到）
    SignalMsg remote_desc;                       // 对端 offer/answer
    std::vector<std::string> peer_candidates;    // 尚未应用的 trickle 候选
    bool remote_creds_applied = false;           // 是否已设置远端凭证
    IceTransport* ice_ptr = nullptr;             // 供消息回调直接应用候选

    // ---- 视频相关（提前声明，因为信令回调要捕获它们）----
    // 视频开关由 opt 决定；如果设备打开失败会自动降级为纯音频
    std::atomic<bool> video_active{false};
    std::atomic<bool> need_keyframe{false};      // 对端请求关键帧（PLI）
    std::atomic<int>  video_tx{0};
    std::atomic<int>  video_rx{0};
    std::atomic<int>  kf_req_sent{0};
    std::atomic<int>  kf_req_recv{0};

    sig.set_on_message([&](const SignalMsg& m) {
        std::lock_guard<std::mutex> lk(mu);
        switch (m.type) {
            case SignalType::kJoined:
                joined = true;
                // 服务器把"房间成员数"放在 sdp 字段里（复用该字段传计数）。
                // 1 = 我是第一个进房的（主叫），2 = 我是第二个（被叫）。
                // 注意：不要读 reason 字段 —— 那里是空的。
                my_member_count = atoi(m.sdp.c_str());
                printf("  [信令] 已入房（成员数 %d）\n", my_member_count);
                break;
            case SignalType::kPeerReady:
                peer_ready = true;
                printf("  [信令] 对端就绪\n");
                break;
            case SignalType::kOffer:
                remote_desc = m;
                got_offer = true;
                printf("  [信令] 收到 offer\n");
                break;
            case SignalType::kAnswer:
                remote_desc = m;
                got_answer = true;
                printf("  [信令] 收到 answer\n");
                break;
            case SignalType::kCandidate:
                // Trickle：候选可能在任何时刻到达。
                // 已设远端凭证就直接应用，否则先攒着（凭证到达后统一应用）。
                if (remote_creds_applied && ice_ptr) {
                    ice_ptr->add_remote_candidate_sdp(m.candidate);
                } else {
                    peer_candidates.push_back(m.candidate);
                }
                break;
            case SignalType::kKeyframeRequest:
                // 对端丢帧了，要求我们下一帧发关键帧（对应 RTCP PLI）
                kf_req_recv++;
                need_keyframe.store(true);
                break;
            case SignalType::kPeerLeft:
                peer_left = true;
                printf("  [信令] 对端离开\n");
                break;
            default:
                break;
        }
        cv.notify_all();
    });

    sig.set_on_state([&](bool connected) {
        printf("  [信令] 连接%s\n", connected ? "建立" : "断开");
    });

    if (!sig.connect(opt.signal_url)) {
        fprintf(stderr, "  ✗ 信令连接失败\n");
        return 1;
    }
    if (!sig.wait_connected(8000)) {
        fprintf(stderr, "  ✗ 信令连接超时\n");
        return 1;
    }

    {
        SignalMsg j = SignalMsg::make_join(opt.room);
        sig.send(j);
    }
    {
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::seconds(10), [&] { return joined && peer_ready; })) {
            fprintf(stderr, "  ✗ 入房/配对超时（另一个端点启动了吗？）\n");
            sig.disconnect();
            return 1;
        }
    }
    printf("  ✓ 信令配对完成\n\n");

    // 角色：**先入房的一方（成员数=1）为主叫**。
    // 这与真实 WebRTC 一致（发起方创建 offer），且天然避免双方同时发 offer 的
    // glare 问题——不用看名字、不用协商 polite/impolite。
    const bool is_caller = (my_member_count == 1);
    printf("  [角色] 本端为%s（DTLS %s）\n",
           is_caller ? "主叫" : "被叫",
           is_caller ? "client/active" : "server/passive");

    // ============================================================
    // 阶段 2：ICE 初始化 + DTLS 准备（拿凭证和指纹）
    // ============================================================
    printf("\n--- 阶段 2/6：ICE 候选收集 + DTLS 准备 ---\n");

    IceTransport::Config icfg;
    icfg.stream_id = 1;
    icfg.controlling = is_caller;   // 主控方 = 主叫

    IceTransport ice;
    if (!ice.init(icfg)) {
        fprintf(stderr, "  ✗ ICE 初始化失败\n");
        return 1;
    }
    ice_ptr = &ice;   // 让信令回调能直接应用 trickle 候选

    // Trickle ICE：每个本地候选一产生就发给对端（不等收集完成）
    ice.set_on_trickle_candidate([&](const std::string& cand_sdp) {
        SignalMsg m = SignalMsg::make_candidate(opt.room, cand_sdp, 0);
        sig.send(m);
    });

    // DTLS 两阶段：先 prepare 拿到自己的指纹（要写进 offer/answer）
    DtlsSrtpSession dtls;
    if (!dtls.prepare(is_caller, 20000)) {
        fprintf(stderr, "  ✗ DTLS 准备失败\n");
        return 1;
    }
    const auto my_creds = ice.local_credentials();
    printf("  本端 ICE 凭证: ufrag=%s pwd=%zu 字节\n",
           my_creds.ufrag.c_str(), my_creds.pwd.size());
    printf("  本端 DTLS 指纹: %s...\n", dtls.local_fingerprint().substr(0, 23).c_str());

    // ============================================================
    // 阶段 3：offer / answer 交换（携带 ICE 凭证 + DTLS 指纹）
    // ============================================================
    printf("\n--- 阶段 3/6：offer/answer 协商 ---\n");

    // 构造本端会话描述（真实 WebRTC 这里是 SDP，本项目用结构化字段 + 简化 sdp 串）
    SignalMsg local_desc;
    local_desc.room        = opt.room;
    local_desc.sdp         = "codec=opus;rate=16000;ch=1;frame=20;ptime=60";
    local_desc.ice_ufrag   = my_creds.ufrag;
    local_desc.ice_pwd     = my_creds.pwd;
    local_desc.fingerprint = dtls.local_fingerprint();
    local_desc.codec       = "opus";
    local_desc.sample_rate = 16000;
    local_desc.channels    = 1;
    local_desc.frame_ms    = 20;

    if (is_caller) {
        local_desc.type = SignalType::kOffer;
        sig.send(local_desc);
        printf("  已发送 offer\n");
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::seconds(15), [&] { return got_answer || peer_left; })) {
            fprintf(stderr, "  ✗ 等待 answer 超时\n");
            return 1;
        }
        if (peer_left) { fprintf(stderr, "  ✗ 对端已离开\n"); return 1; }
        printf("  ✓ 收到 answer\n");
    } else {
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::seconds(15), [&] { return got_offer || peer_left; })) {
            fprintf(stderr, "  ✗ 等待 offer 超时\n");
            return 1;
        }
        if (peer_left) { fprintf(stderr, "  ✗ 对端已离开\n"); return 1; }
        lk.unlock();
        printf("  收到 offer\n");

        local_desc.type = SignalType::kAnswer;
        sig.send(local_desc);
        printf("  已回 answer\n");
    }

    if (remote_desc.ice_ufrag.empty() || remote_desc.ice_pwd.empty()) {
        fprintf(stderr, "  ✗ 对端未提供 ICE 凭证\n");
        return 1;
    }

    // ============================================================
    // 阶段 4：应用远端描述 + 候选，等 ICE 连通
    // ============================================================
    printf("\n--- 阶段 4/6：ICE 连通性检查 ---\n");

    ice.set_remote_credentials(remote_desc.ice_ufrag, remote_desc.ice_pwd);
    printf("  对端 ICE 凭证: ufrag=%s\n", remote_desc.ice_ufrag.c_str());
    if (!remote_desc.fingerprint.empty()) {
        printf("  对端 DTLS 指纹: %s...\n", remote_desc.fingerprint.substr(0, 23).c_str());
    }

    // 应用已经收到的候选（Trickle 可能早于 offer 到达），
    // 之后新到的候选由信令回调直接应用。
    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& c : peer_candidates) {
            ice.add_remote_candidate_sdp(c);
        }
        printf("  已应用 %zu 个对端候选（后续候选随到随用）\n", peer_candidates.size());
        peer_candidates.clear();
        remote_creds_applied = true;
    }

    // 等候选收集完成（本地候选都已发出）
    ice.wait_gathering_done(8000);

    if (!ice.wait_connected(15000)) {
        fprintf(stderr, "  ✗ ICE 连通超时（候选对未选通）\n");
        return 1;
    }
    printf("  ✓ ICE 通道已连通（选中候选对: %s）\n", ice.selected_pair().c_str());

    // ============================================================
    // 阶段 5：DTLS 握手 + SRTP 密钥派生
    // ============================================================
    printf("\n--- 阶段 5/6：DTLS 握手 ---\n");

    IceDtlsTransport dtls_transport(&ice);

    // ★ 关键接线顺序：必须在握手之前把 ICE 的接收回调接上，
    //   否则握手期间到达的 DTLS 记录无人接收 → 必然超时。
    //   见 docs/02-pitfalls.md 坑 29。
    std::atomic<bool> srtp_ready{false};
    std::atomic<int>  media_rx{0};
    std::atomic<int>  srtp_fail{0};

    VirtualAudioDevice::Config dcfg;
    dcfg.source_path = opt.audio_file;
    dcfg.sink_path   = opt.out_file;
    VirtualAudioDevice device(dcfg);
    if (!device.open()) {
        fprintf(stderr, "  ✗ 虚拟音频设备打开失败\n");
        return 1;
    }

    AudioPipeline pipe;

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
            fprintf(stderr, "  ! 虚拟视频设备打开失败，降级为纯音频\n");
        }
    }

    // 接收路径：ICE -> 分流 -> SRTP 解密 -> 解析包头 -> 喂给管线
    ice.set_on_recv([&](const uint8_t* data, size_t len) {
        if (!looks_like_rtp(data, len)) {
            // DTLS 记录（握手阶段）
            dtls_transport.feed(data, len);
            return;
        }
        if (!srtp_ready.load()) {
            // 握手还没完成就到了 RTP：说明对端已就绪，我方可能快了/慢了
            srtp_fail++;
            return;
        }
        std::vector<uint8_t> buf(data, data + len);
        int n = int(buf.size());
        if (!dtls.unprotect(buf.data(), &n)) {
            srtp_fail++;
            return;
        }
        PacketHeader ph;
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
    });

    // 双方并发握手（DTLS 是双向多轮协议）
    std::atomic<bool> dtls_ok{false};
    std::thread dtls_thread([&] {
        dtls_ok = dtls.handshake(&dtls_transport, remote_desc.fingerprint);
    });

    // 握手期间持续把 DTLS 记录喂进来（握手线程自己会从 dtls_transport 收）
    dtls_thread.join();

    if (!dtls_ok.load()) {
        fprintf(stderr, "  ✗ DTLS 握手失败\n");
        return 1;
    }
    srtp_ready = true;
    printf("  ✓ DTLS 握手完成，SRTP 密钥已派生\n");
    printf("    对端指纹校验: %s\n",
           remote_desc.fingerprint.empty() ? "跳过（对端未提供）" : "通过");

    // ============================================================
    // 阶段 6：启动媒体管线，开始真实通话
    // ============================================================
    printf("\n--- 阶段 6/6：媒体传输 ---\n");

    AudioPipeline::Config pcfg;
    pcfg.session_id       = 1;
    pcfg.bitrate_bps      = opt.bitrate;
    pcfg.jb_target_ms     = opt.jb_ms;
    pcfg.aggregate_frames = 3;

    if (!pipe.init(pcfg, &device)) {
        fprintf(stderr, "  ✗ 音频管线初始化失败\n");
        return 1;
    }

    std::atomic<uint64_t> tx_pkts{0};
    std::atomic<uint64_t> tx_bytes{0};

    // 发送路径：管线产出包 -> 序列化 -> SRTP 加密 -> ICE 发出
    pipe.set_send_fn([&](const PacketHeader& h, const std::vector<uint8_t>& payload) {
        uint8_t buf[2048];
        const int n = Packet::serialize(h, payload.data(), payload.size(), buf, sizeof(buf));
        if (n <= 0) return;
        int len = n;
        if (!dtls.protect(buf, &len)) return;
        if (ice.send(buf, size_t(len))) {
            tx_pkts++;
            tx_bytes += uint64_t(len);
        }
    });

    // ---- 视频管线初始化与接线 ----
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
            fprintf(stderr, "  ! 视频管线初始化失败，降级为纯音频\n");
            video_ready = false;
        } else {
            // ★ 初始化完成就置 active —— 不要等到 start() 之后。
            //   原因：ICE 接收回调靠 video_active 判断是否把分片交给视频管线。
            //   若置得太晚，对端早到的分片（正好包含**首个关键帧**）会被静默丢弃，
            //   表现为主叫端固定丢约 15 帧且不触发 PLI（因为根本没走到解码）。
            //   见 docs/02-pitfalls.md 坑 47。
            video_active = true;
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
            printf("  视频: %dx%d @%dfps, %dkbps, session_id=2, 源=%s\n",
                   opt.video_width, opt.video_height, opt.video_fps,
                   opt.video_bitrate_kbps, opt.video_source.c_str());
        }
    }

    pipe.start_playout();     // 启动播放（抖动缓冲 -> 解码 -> 写设备）
    device.start_playout();   // 启动设备消费线程（模拟 DMA 取走数据）
    pipe.start_sending();     // 启动采集+编码+发送

    printf("  ▶ 通话中（%d 秒）... 本端采集 -> 编码 -> 加密 -> ICE -> 对端\n", opt.seconds);
    printf("    %s\n", opt.play_audio
        ? "（对端音频会被解码并播放，可以听到）"
        : "（未开启播放）");

    // 实时进度：每秒打印一行。
    //
    // ★ 循环粒度是 100ms，不是 1s —— 这一点很关键。
    //
    //   这里除了打印统计，还兼任"控制消息泵"：处理对端发来的
    //   关键帧请求（PLI）。如果循环是 1 秒一次，一个 PLI 最多要等
    //   1 秒才被应用，期间接收端持续丢帧 —— 实测正好丢 15 帧
    //   （15fps × 1s），表现为"主叫端固定丢 15 帧，被叫端 0 丢"。
    //
    //   控制消息的响应延迟应该由它的业务需要决定（PLI 希望 <1 帧），
    //   而不是被"统计打印的频率"绑架。见 docs/02-pitfalls.md 坑 47。
    const int kTickMs = 100;
    const int kTicksPerSec = 1000 / kTickMs;   // 10
    const long total_ticks = long(opt.seconds) * kTicksPerSec;

    for (long tick = 0; tick < total_ticks; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));

        // 控制消息泵（每 100ms 检查一次）：处理对端的关键帧请求
        //
        // 注意判断顺序：**先判断 video_active，再清 flag**。
        // 若写成 `need_keyframe.exchange(false) && video_active.load()`，
        // exchange 会先把 flag 清掉，video 未激活时这个请求就被静默吞了 ——
        // 对端会一直等一个永远不会来的关键帧。见 docs/02-pitfalls.md 坑 47。
        if (video_active.load() && need_keyframe.load()) {
            need_keyframe.store(false);
            vpipe.on_keyframe_request();
        }

        // 只在整秒时打印统计
        if ((tick + 1) % kTicksPerSec != 0) continue;
        const int s = int((tick + 1) / kTicksPerSec) - 1;

        auto rep = pipe.report();
        if (video_active.load()) {
            auto vrep = vpipe.report();
            printf("    [%2ds] 音频 %llu/%llu 包 %.0fkbps | 视频 %llu/%llu 帧 %.0fkbps "
                   "解码%llu 丢弃%llu PLI发%llu收%llu\n",
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
            printf("    [%2ds] 发送 %llu 包 / 接收 %llu 包 | 播放 %llu 帧 | PLC %llu | %.1f kbps\n",
                   s + 1,
                   (unsigned long long)rep.sent_packets,
                   (unsigned long long)rep.recv_packets,
                   (unsigned long long)rep.jb.played,
                   (unsigned long long)rep.jb.plc,
                   rep.cur_bitrate_kbps);
        }
        fflush(stdout);
    }

    // 收尾：先停管线再停设备（视频同理）
    if (video_active.load()) {
        vpipe.stop();
        vdevice.stop_capture();
        vdevice.stop_playout();
        video_active = false;
    }
    pipe.stop_playout();
    device.stop_playout();
    pipe.stop_sending();

    // ============================================================
    // 结果
    // ============================================================
    const auto rep = pipe.report();
    printf("\n--- 通话结果 ---\n");
    printf("  发送包数      : %llu（%llu 字节）\n",
           (unsigned long long)rep.sent_packets, (unsigned long long)tx_bytes.load());
    printf("  接收包数      : %llu\n", (unsigned long long)rep.recv_packets);
    printf("  SRTP 加密     : %llu 包\n", (unsigned long long)dtls.stats().protected_pkts);
    printf("  SRTP 解密     : %llu 包（失败 %llu）\n",
           (unsigned long long)dtls.stats().unprotected_pkts,
           (unsigned long long)dtls.stats().unprotect_failed);
    printf("  抖动缓冲接收  : %llu 帧\n", (unsigned long long)rep.jb.received);
    printf("  抖动缓冲播放  : %llu 帧\n", (unsigned long long)rep.jb.played);
    printf("  PLC 补偿      : %llu 帧\n", (unsigned long long)rep.jb.plc);
    printf("  目标延迟      : %.0f ms\n", rep.jb.cur_target_ms);
    printf("  实测码率      : %.1f kbps\n", rep.cur_bitrate_kbps);
    printf("  解码错误      : %llu\n", (unsigned long long)rep.decode_errors);

    // ---- 视频统计 ----
    if (opt.video) {
        const auto vrep = vpipe.report();
        printf("\n  [视频]\n");
        printf("  采集/编码帧   : %llu / %llu（关键帧 %llu）\n",
               (unsigned long long)vrep.frames_captured,
               (unsigned long long)vrep.frames_encoded,
               (unsigned long long)vrep.keyframes_sent);
        printf("  发送分片      : %llu（%llu 字节，实测 %.0f kbps）\n",
               (unsigned long long)vrep.fragments_sent,
               (unsigned long long)vrep.bytes_sent, vrep.actual_kbps);
        printf("  接收分片/完整帧: %llu / %llu\n",
               (unsigned long long)vrep.fragments_recv,
               (unsigned long long)vrep.frames_recv);
        printf("  解码/丢弃帧   : %llu / %llu\n",
               (unsigned long long)vrep.frames_decoded,
               (unsigned long long)vrep.frames_dropped);
        printf("  关键帧请求    : 发出 %llu / 收到 %llu（PLI）\n",
               (unsigned long long)vrep.keyframe_requests,
               (unsigned long long)kf_req_recv.load());
        printf("  收帧率        : %.1f fps（发送 %.1f fps）\n",
               vrep.fps_in, vrep.fps_out);
        if (!opt.video_sink_dir.empty()) {
            printf("  解码帧落盘    : %s（%llu 帧 PPM）\n",
                   opt.video_sink_dir.c_str(),
                   (unsigned long long)vdevice.stats().sink_frames);
        }
    }
    printf("  播放落盘      : %s（%.1f 秒）\n", opt.out_file.c_str(),
           double(device.play_stats().played_samples) / 16000.0);

    const bool ok = rep.sent_packets > 0 &&
                    dtls.stats().protected_pkts > 0 &&
                    dtls.stats().unprotected_pkts > 0 &&
                    rep.decode_errors == 0;

    printf("\n%s\n", ok
        ? "✅ 通话成功：双向音频已通过 ICE + DTLS-SRTP 真实传输"
        : "❌ 通话未完成：检查上面哪一阶段失败");

    if (ok && opt.play_audio) {
        printf("   试听：把 %s 转成 WAV 后播放\n", opt.out_file.c_str());
        printf("         python3 scripts/pcm2wav.py %s /tmp/call.wav 16000 && paplay /tmp/call.wav\n",
               opt.out_file.c_str());
    }

    device.close();
    sig.disconnect();
    return ok ? 0 : 1;
}

}  // namespace rtc
