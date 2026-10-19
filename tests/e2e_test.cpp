// 端到端集成测试：音频经 ICE + DTLS-SRTP 真实通道传输
//
// 这是本项目的"验收测试"——把前五个阶段的能力串成一条完整链路：
//
//   采集(虚拟设备) -> Opus 编码 -> 打包(自定义头) -> NACK/FEC 保护
//     -> SRTP 加密 -> ICE 通道(UDP) -> ──网络──>
//     -> ICE 接收 -> SRTP 解密 -> NACK/FEC 恢复 -> 抖动缓冲
//     -> Opus 解码 -> 播放(虚拟设备)
//
// 与 WebRTC 的完整对应关系：
//   ADM          -> VirtualAudioDevice
//   Opus         -> OpusCodec
//   RTP          -> Packet（16 字节头 + seq/ts/trans_id）
//   NACK/ULPFEC  -> NackRequester / FecEncoder
//   DTLS-SRTP    -> DtlsSrtpSession
//   ICE/STUN     -> IceTransport
//   NetEq        -> JitterBuffer
//   GCC          -> CongestionController
//
// 验证目标：
//   1. ICE 候选交换 → 候选对选通
//   2. DTLS 握手 → SRTP 密钥就绪（双向指纹互验）
//   3. 加密音频经真实 UDP 通道传输
//   4. 接收端解密 → 抖动缓冲 → 解码，内容与发送端一致
//   5. 丢包场景下 NACK/FEC 的恢复效果
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/util.h"
#include "crypto/dtls_srtp.h"
#include "ice/ice_transport.h"
#include "media/jitter_buffer.h"
#include "media/opus_codec.h"
#include "metrics/metrics.h"
#include "net/loss_protection.h"
#include "protocol/packet.h"

using namespace rtc;

static int g_fail = 0;
// 条件只求值一次（避免副作用表达式执行两遍）。见 docs/02-pitfalls.md 坑 27。
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

// 把 IceTransport 适配成 DtlsTransport（DTLS 记录走 ICE 通道）
class IceDtlsTransport : public DtlsTransport {
public:
    explicit IceDtlsTransport(IceTransport* ice) : ice_(ice) {}

    int send(const uint8_t* data, size_t len) override {
        if (!ice_->send(data, len)) return -1;
        return int(len);
    }

    int recv(uint8_t* buf, size_t cap, int timeout_ms) override {
        // ★ 必须真正等待 timeout_ms，而不是"队列空就立刻返回 0"。
        //
        // DTLS 握手是双向多轮往返：SSL_do_handshake 返回 WANT_READ 后，
        // 调用方会再次调 recv 等待对端报文。如果 recv 立即返回 0，
        // 外层循环会变成忙等（busy loop），在极短的真实时间内把
        // handshake_timeout 的"逻辑时间预算"耗光，握手直接超时失败。
        // 见 docs/02-pitfalls.md 坑 29。
        const int64_t deadline = now_ms() + timeout_ms;
        while (now_ms() <= deadline) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!queue_.empty()) {
                    size_t n = std::min(cap, queue_.size());
                    for (size_t i = 0; i < n; i++) {
                        buf[i] = queue_.front();
                        queue_.pop_front();
                    }
                    return int(n);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;   // 真超时
    }

    void feed(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (size_t i = 0; i < len; i++) queue_.push_back(data[i]);
    }

    bool connected() const { return ice_->connected(); }
    uint64_t rx_count() const { return ice_->rx_count(); }
    uint64_t tx_count() const { return ice_->tx_count(); }

private:
    IceTransport*       ice_;
    std::deque<uint8_t> queue_;
    std::mutex          mutex_;
};

int main(int argc, char** argv) {
    double loss_rate = 0.0;
    int    seconds = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--loss") && i + 1 < argc) loss_rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ 端到端集成测试 ################\n");
    printf("链路: 采集 -> Opus -> 打包 -> SRTP -> ICE(UDP) -> 解密 -> 抖动缓冲 -> 解码 -> 播放\n");
    printf("模拟丢包率: %.1f%%\n\n", loss_rate * 100);

    // ================= 1. ICE 候选交换 =================
    printf("=== 1. ICE 候选收集与交换 ===\n");
    IceTransport::Config ica;
    ica.stream_id = 1;
    ica.controlling = true;

    IceTransport ice_a, ice_b;
    CHECK(ice_a.init(ica), "A 侧 ICE 初始化");
    IceTransport::Config icb = ica;
    icb.controlling = false;
    CHECK(ice_b.init(icb), "B 侧 ICE 初始化");
    CHECK(ice_a.wait_gathering_done(8000), "A 候选收集完成");
    CHECK(ice_b.wait_gathering_done(8000), "B 候选收集完成");

    CHECK(ice_b.set_remote_sdp(ice_a.local_sdp()), "B 设置 A 候选");
    CHECK(ice_a.set_remote_sdp(ice_b.local_sdp()), "A 设置 B 候选");
    CHECK(ice_a.wait_connected(10000), "A 候选对选通");
    CHECK(ice_b.wait_connected(10000), "B 候选对选通");

    // ================= 2. DTLS-SRTP 握手（在 ICE 通道上） =================
    printf("\n=== 2. DTLS-SRTP 握手（在 ICE 通道上）===\n");

    // 先声明 DTLS 传输适配器与 SRTP 会话（下面的 lambda 需要捕获它们）
    IceDtlsTransport ta(&ice_a), tb(&ice_b);
    DtlsSrtpSession   sa, sb;
    std::atomic<bool> a_ok{false}, b_ok{false};

    // ★ 关键顺序：**必须在握手之前**把 ICE 的接收回调接上。
    //
    // 早期版本把 set_on_recv 放在握手之后，导致握手期间到达的 DTLS 记录
    // 无人接收（回调还没注册），双方都等不到对方报文 → 握手必然超时。
    // 这不是 DTLS 的问题，而是"接线顺序"错了。
    // 见 docs/02-pitfalls.md 坑 29。
    std::atomic<int> recv_packets{0};
    std::atomic<int> recv_frames{0};
    std::atomic<int> decoded_ok{0};
    std::atomic<int> decrypted_fail{0};
    std::vector<int16_t> last_decoded;
    std::atomic<int> max_decoded_amp{0};   // 所有解码帧中的最大幅度

    OpusCodec codec_a, codec_b;
    JitterBuffer jb;
    NackRequester nack;
    FecEncoder fec_enc(FecEncoder::Config{4, 1});
    FecDecoder fec_dec;
    MetricsCollector metrics;      // 统一指标采集（对应 WebRTC getStats）

    // 拥塞控制（对应 GCC）：根据延迟梯度与丢包率自适应调码率。
    // 输出接到编码器上，形成"测量 -> 决策 -> 生效"的闭环。
    CongestionController::Config ccfg;
    ccfg.start_bitrate_bps = 16000;
    ccfg.min_bitrate_bps = 6000;
    ccfg.max_bitrate_bps = 48000;
    CongestionController gcc(ccfg);
    std::atomic<int> last_adaptive_bps{16000};

    // 统一的接收处理：先看是不是 SRTP（RTP 版本位 = 2），
    // 不是则当作 DTLS 记录交给握手侧。
    auto on_ice_recv = [&](const uint8_t* data, size_t len) {
        LOGD("e2e", "B: ICE recv %zu bytes, first=0x%02x", len, len ? data[0] : 0);
        const bool looks_rtp = (len > 12) && ((data[0] & 0xC0) == 0x80);
        if (looks_rtp) {
            std::vector<uint8_t> buf(data, data + len);
            int blen = int(buf.size());
            if (sb.unprotect(buf.data(), &blen)) {
                metrics.on_srtp(false, true);
                PacketHeader ph;
                std::vector<uint8_t> pp;
                if (Packet::parse(buf.data(), size_t(blen), &ph, &pp)) {
                    recv_packets++;
                    metrics.on_packet_received(size_t(blen));

                    if (ph.flags & kFlagFecPacket) {
                        // ★ FEC 包用独立 SSRC（session_id=2），序号空间与媒体流不同。
                        //   只能喂给 FEC 解码器，**绝不能**喂给 NACK 统计 ——
                        //   否则两个序号空间混在一起，丢包统计会算错。
                        //
                        //   这个错误实际发生过（本文件早期版本把 nack.on_packet
                        //   放在分支判断之前，所有包都进了 NACK）：
                        //   25% 丢包时 "实际丢弃 9" 却只 "检测丢包 6" —— 少报，
                        //   因为 NACK 看到的序号序列被 FEC 包搅乱了。
                        //   见 docs/02-pitfalls.md 坑 34。
                        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> rec;
                        fec_dec.on_fec(pp.data(), pp.size(), &rec);
                        metrics.on_recovered_fec(fec_dec.stats().recovered);
                    } else if (ph.session_id == 1) {
                        // 媒体流：喂给 NACK 统计 + FEC 源包记录 + 抖动缓冲
                        nack.on_packet(ph.seq, -1);
                        fec_dec.on_source(ph.seq, pp.data(), pp.size());
                        std::vector<std::vector<uint8_t>> frames;
                        if (OpusCodec::unpack_frames(pp.data(), int(pp.size()), &frames) >= 0) {
                            for (size_t i = 0; i < frames.size(); i++) {
                                jb.push(uint16_t(ph.seq * frames.size() + i),
                                        frames[i].data(), frames[i].size());
                            }
                        }
                    }
                }
            } else {
                decrypted_fail++;
                metrics.on_srtp(false, false);
            }
        } else {
            // DTLS 记录：喂给握手侧
            tb.feed(data, len);
        }
    };

    ice_b.set_on_recv(on_ice_recv);
    // ★ A 侧也必须把 DTLS 记录喂给 ta —— DTLS 是双向握手，
    // client 同样需要接收 server 的证书与 Finished。
    // 早期只接了 B 侧，导致 A 侧收不到任何握手报文。
    // 见 docs/02-pitfalls.md 坑 29。
    ice_a.set_on_recv([&ta](const uint8_t* data, size_t len) { ta.feed(data, len); });

    std::thread th_a([&] {
        DtlsSrtpSession::Config c;
        c.is_client = true;
        c.handshake_timeout_ms = 20000;
        a_ok = sa.handshake(&ta, c);
    });
    std::thread th_b([&] {
        DtlsSrtpSession::Config c;
        c.is_client = false;
        c.handshake_timeout_ms = 20000;
        b_ok = sb.handshake(&tb, c);
    });
    th_a.join();
    th_b.join();

    CHECK(a_ok.load() && b_ok.load(), "双方 DTLS 握手成功");
    CHECK(sa.ready() && sb.ready(), "SRTP 会话就绪");
    sa.refresh_peer_fingerprint();
    sb.refresh_peer_fingerprint();
    CHECK(sa.peer_fingerprint() == sb.local_fingerprint(), "A 验证 B 的证书指纹");
    CHECK(sb.peer_fingerprint() == sa.local_fingerprint(), "B 验证 A 的证书指纹");

    // ================= 3. 音频链路准备 =================
    printf("\n=== 3. 音频链路建立 ===\n");
    CHECK(codec_a.init(16000, 0, false, true), "A 编码器初始化（开启 in-band FEC）");
    CHECK(codec_b.init(16000, 0, false, true), "B 解码器初始化");

    // ================= 4. 发送加密音频 =================
    printf("\n=== 4. 加密音频传输（%d 秒，丢包率 %.1f%%）===\n", seconds, loss_rate * 100);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    const int frame_samples = OpusCodec::kFrameSamples;
    std::vector<int16_t> pcm(frame_samples);
    uint16_t seq = 0;
    uint32_t ts = 0;
    std::atomic<bool> stop{false};
    std::atomic<int>  sent_ok{0};
    std::atomic<int>  dropped{0};
    std::atomic<int>  fec_sent{0};

    std::thread sender([&] {
        int64_t next = now_ms();
        std::vector<std::vector<uint8_t>> group;   // 攒 3 帧做聚合
        while (!stop) {
            // 生成一帧正弦波（相位连续，模拟真实音频）
            static double phase = 0;
            const double step = 2.0 * M_PI * 440.0 / 16000.0;
            for (int i = 0; i < frame_samples; i++) {
                pcm[i] = int16_t(8000 * sin(phase));
                phase += step;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
            }

            uint8_t enc[OpusCodec::kMaxFrameBytes];
            metrics.on_frame_captured();
            int n = codec_a.encode(pcm.data(), frame_samples, enc, sizeof(enc));
            metrics.on_frame_encoded(n > 0, n > 0 ? size_t(n) : 0);
            if (n > 0) {
                group.emplace_back(enc, enc + n);
            }

            // 每 3 帧聚合成一个包（60ms）
            if (group.size() >= 3) {
                std::vector<uint8_t> payload(OpusCodec::kMaxPacketBytes);
                int plen = OpusCodec::pack_frames(group, payload.data(), int(payload.size()));
                group.clear();
                if (plen <= 0) continue;
                payload.resize(size_t(plen));

                PacketHeader h;
                h.session_id = 1;
                h.seq = seq++;
                h.timestamp = ts;
                h.phase = Phase::kBody;
                h.flags = kFlagNone;
                h.trans_id = 1;
                h.payload_len = uint16_t(payload.size());
                ts += uint32_t(frame_samples * 3);

                // 序列化自定义包头
                uint8_t wire[2048];
                int total = Packet::serialize(h, payload.data(), payload.size(),
                                              wire, sizeof(wire));
                if (total <= 0) continue;

                // 拥塞控制闭环：把"包的到达情况"反馈给 GCC，由它调目标码率。
                //
                // 这里用简化但真实的反馈：以发送节拍作为 send_ms，
                // 以当前时间作为 recv_ms（同机测试二者接近，梯度会反映排队趋势）。
                // 真实部署中 recv_ms 来自接收端的 RTCP 反馈。
                gcc.on_arrival(next, now_ms());
                gcc.on_loss_rate(loss_rate);
                const int target = gcc.update_bitrate(now_ms());
                last_adaptive_bps = target;
                // 把目标码率作用到编码器上（形成闭环）
                codec_a.set_bitrate(target);

                // 按丢包率动态调整 FEC 冗余率（工程上的关键权衡）：
                //   <5%   丢包：靠 NACK 就够，冗余降到 k=8（12.5% 开销）
                //   5~15% 丢包：k=4（25% 开销）
                //   >15%  丢包：k=2（50% 开销，优先保连续性）
                // 这是"带宽 vs 抗损"的动态取舍，对应 WebRTC 的
                // protect_bitrate / FEC overhead 自适应。
                if (loss_rate < 0.05)       fec_enc.set_group_size(8);
                else if (loss_rate < 0.15)  fec_enc.set_group_size(4);
                else                        fec_enc.set_group_size(2);

                // FEC：按当前动态冗余率生成冗余包
                std::vector<uint8_t> fec_payload;
                bool has_fec = fec_enc.add_source(h.seq, payload.data(),
                                                  payload.size(), &fec_payload);

                // 模拟网络丢包：源包按概率丢
                if (uni(rng) < loss_rate) {
                    dropped++;
                } else {
                    // SRTP 加密后经 ICE 发出
                    uint8_t srtp_buf[2048];
                    memcpy(srtp_buf, wire, size_t(total));
                    int slen = total;
                    if (sa.protect(srtp_buf, &slen)) {
                        metrics.on_srtp(true, true);
                        if (ice_a.send(srtp_buf, size_t(slen))) {
                            sent_ok++;
                            metrics.on_packet_sent(size_t(total), false);
                        }
                    } else {
                        metrics.on_srtp(true, false);
                    }
                }

                // FEC 包独立发送。
                //
                // ★ 必须用**独立的 session_id（映射到独立 SSRC）**，而不是
                //   "源序号 + 10000" 这种方式。
                //   原因：SRTP 按 SSRC 跟踪序列号。若 FEC 包用 10000+ 的序号，
                //   会让同一 SSRC 的序列号先跳到 10000 再回落到个位数，
                //   被 SRTP 判为重放而拒绝；接收端的丢包统计也会把这 10000 的跳变
                //   算成"丢了 1 万个包"（实测 检测丢包=10032，全是假的）。
                //   见 docs/02-pitfalls.md 坑 30。
                if (has_fec) {
                    if (uni(rng) >= loss_rate) {
                        PacketHeader fh = h;
                        fh.session_id = 2;      // ← 独立 SSRC，与媒体流分开
                        fh.flags = kFlagFecPacket;
                        uint8_t fwire[2048];
                        int ftotal = Packet::serialize(fh, fec_payload.data(),
                                                       fec_payload.size(), fwire, sizeof(fwire));
                        if (ftotal > 0) {
                            int flen = ftotal;
                            if (sa.protect(fwire, &flen)) {
                                metrics.on_srtp(true, true);
                                if (ice_a.send(fwire, size_t(flen))) {
                                    fec_sent++;
                                    metrics.on_packet_sent(size_t(ftotal), true);
                                }
                            } else {
                                metrics.on_srtp(true, false);
                            }
                        }
                    }
                }
            }

            // 按 20ms 节拍推进（每帧编码一次，但每 3 帧才发一个包）
            next += 20;
            std::this_thread::sleep_until(
                std::chrono::steady_clock::time_point(std::chrono::milliseconds(next)));
        }
    });

    // 接收端播放线程：从抖动缓冲取帧、解码
    std::thread player([&] {
        std::vector<uint8_t> payload;
        std::vector<int16_t> out(frame_samples);
        int64_t next = now_ms();
        while (!stop) {
            if (jb.ready_to_play(now_ms())) {
                payload.clear();
                if (jb.pop(&payload, now_ms())) {
                    int n;
                    if (payload.empty()) {
                        n = codec_b.decode(nullptr, 0, out.data(), frame_samples);   // PLC
                    } else {
                        n = codec_b.decode(payload.data(), int(payload.size()),
                                           out.data(), frame_samples);
                    }
                    if (n > 0) {
                        decoded_ok++;
                        metrics.on_frame_decoded(true);
                        last_decoded = out;
                        // ★ 幅度检查必须看**所有**解码帧的最大值，而不是只看最后一帧。
                        //   最后一帧可能正好是 PLC 产生的静音帧（PLC 用舒适噪声，
                        //   幅度接近 0），导致误判为"解码出静音"。
                        //   见 docs/02-pitfalls.md 坑 30。
                        for (int i = 0; i < n; i++) {
                            const int a = abs(int(out[i]));
                            if (a > max_decoded_amp) max_decoded_amp = a;
                        }
                    } else {
                        metrics.on_frame_decoded(false);
                    }
                }
            }
            next += 20;
            std::this_thread::sleep_until(
                std::chrono::steady_clock::time_point(std::chrono::milliseconds(next)));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    sender.join();
    player.join();

    // ================= 5. 结果统计 =================
    printf("\n=== 5. 端到端结果 ===\n");
    auto jst = jb.snapshot();
    auto nst = nack.snapshot();

    printf("  发送包数(源)      : %d\n", sent_ok.load());
    printf("  发送包数(FEC)     : %d\n", fec_sent.load());
    printf("  模拟丢弃(源)      : %d\n", dropped.load());
    printf("  接收并解密包数    : %d\n", recv_packets.load());
    printf("  解密失败次数      : %d\n", decrypted_fail.load());
    printf("  抖动缓冲接收帧    : %llu\n", (unsigned long long)jst.received);
    printf("  抖动缓冲播放帧    : %llu\n", (unsigned long long)jst.played);
    printf("  PLC 补偿次数      : %llu\n", (unsigned long long)jst.plc);
    printf("  解码成功帧数      : %d\n", decoded_ok.load());
    printf("  检测丢包          : %llu\n", (unsigned long long)nst.lost_detected);
    printf("  FEC 恢复          : %llu\n", (unsigned long long)fec_dec.stats().recovered);
    printf("  SRTP 加密/解密    : %llu / %llu\n",
           (unsigned long long)sa.stats().protected_pkts,
           (unsigned long long)sb.stats().unprotected_pkts);
    // ---- 拥塞控制闭环结果 ----
    const auto ccst = gcc.snapshot();
    printf("  [拥塞控制] 起始/最终码率: 16000 -> %d bps\n", last_adaptive_bps.load());
    printf("  [拥塞控制] 延迟梯度=%.1f ms, 状态=%s, 增%llu/降%llu 次\n",
           ccst.delay_gradient_ms, CongestionController::state_name(ccst.state),
           (unsigned long long)ccst.increase_events,
           (unsigned long long)ccst.decrease_events);
    printf("  [拥塞控制] 编码器当前码率: %d bps\n", codec_a.bitrate());

    // 关键断言
    CHECK(sent_ok.load() > 0, "发送了加密音频包");
    CHECK(recv_packets.load() > 0, "接收端解密并解析了包");
    CHECK(sb.stats().unprotected_pkts > 0, "SRTP 解密成功");
    CHECK(decoded_ok.load() > 0, "解码出了音频帧");
    CHECK(!last_decoded.empty(), "得到了解码后的 PCM 数据");
    CHECK(decrypted_fail.load() == 0 || loss_rate > 0,
          "无丢包时解密失败次数为 0（有丢包时容忍）");

    // 内容正确性：检查**所有**解码帧的最大幅度（而不是只看最后一帧——
    // 最后一帧可能是 PLC 产生的静音帧，幅度接近 0）
    const int max_amp = max_decoded_amp.load();
    printf("  解码后最大幅度    : %d\n", max_amp);
    CHECK(max_amp > 1000, "解码出的音频有合理幅度（非静音）");

    // ================= 6. 统一指标统计 =================
    //
    // 对应 WebRTC 的 getStats()：把各层数据汇总成一份可上报的指标。
    printf("\n=== 6. 统一指标统计（对应 getStats）===\n");
    metrics.on_jb_stats(jst.received, jst.played, jst.plc, jst.late,
                        jst.cur_target_ms);
    metrics.on_jitter(jst.avg_jitter_ms);
    metrics.on_packets_lost(nst.lost_detected);
    metrics.on_recovered_nack(nst.recovered);
    metrics.on_nack_request(nst.nack_sent);
    // 上报拥塞控制状态（闭环的可见性部分）
    metrics.on_congestion(last_adaptive_bps.load(),
                          gcc.snapshot().delay_gradient_ms);
    metrics.on_ice(ice_a.tx_count(), ice_b.rx_count(),
                   ice_a.connected() && ice_b.connected());

    CallMetrics cm = metrics.snapshot();
    printf("%s\n", cm.to_string().c_str());

    // 指标自洽性校验（跨层交叉验证）
    printf("  -- 跨层自洽性校验 --\n");
    printf("    发送包(SRTP加密)=%llu  接收包(SRTP解密)=%llu\n",
           (unsigned long long)cm.srtp_protected,
           (unsigned long long)cm.srtp_unprotected);
    CHECK(cm.srtp_protected == cm.srtp_unprotected,
          "SRTP 加密次数 == 解密次数（无丢包时逐包对应）");
    CHECK(cm.packets_sent + cm.fec_packets_sent == cm.srtp_protected,
          "发出的媒体+FEC 包数 == SRTP 加密次数");

    const double mrec = cm.recovery_rate();
    printf("    恢复率=%.1f%%  仍缺失=%llu\n",
           mrec * 100.0, (unsigned long long)cm.still_missing());
    CHECK(mrec >= 0.0 && mrec <= 1.0, "恢复率落在 [0,1] 区间");

    // 拥塞控制闭环校验：目标码率必须非零且落在配置范围内
    printf("    拥塞控制目标码率=%d bps\n", cm.target_bitrate_bps);
    CHECK(cm.target_bitrate_bps >= 6000 && cm.target_bitrate_bps <= 48000,
          "拥塞控制目标码率落在 [6000, 48000] 内");
    CHECK(cm.target_bitrate_bps == last_adaptive_bps.load(),
          "指标里的目标码率 == 拥塞控制的输出（闭环已接通）");

    const double mcont = cm.playout_continuity();
    printf("    播放连续性=%.1f%%\n", mcont * 100.0);
    CHECK(mcont > 0.5, "播放连续性 > 50%");

    // JSON 输出可被上报
    const std::string js = cm.to_json();
    CHECK(js.find("\"bitrate_bps\"") != std::string::npos, "指标可导出为 JSON");
    printf("    JSON 长度: %zu 字节\n", js.size());

    ice_a.close();
    ice_b.close();

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
