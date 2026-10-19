// DTLS-SRTP 测试：验证握手、密钥派生、加解密与防篡改
//
// 用一对进程内的内存管道模拟"两个端点之间的传输通道"
// （真实部署时这条通道是 ICE 选通的候选对）。
//
// 验证内容：
//   1. DTLS 1.2 握手完成
//   2. 双方证书指纹一致（防 MITM 的基础）
//   3. RFC 5764 密钥派生：双方 client/server key 一致
//   4. SRTP 加密 -> 传输 -> 解密，载荷还原
//   5. 篡改密文后解密失败（认证保护有效）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "crypto/dtls_srtp.h"
#include "common/util.h"

using namespace rtc;

static int g_fail = 0;
// ★ 关键：条件必须**只求值一次**，先存进局部变量再用于打印和计数。
//
// 早期版本写成：
//   #define CHECK(c, m) do { printf("%s", (c) ? "PASS" : "FAIL"); if (!(c)) g_fail++; } while (0)
// 对**带副作用**的表达式（如 CHECK(a.protect(buf, &len), ...)）会执行两遍：
//   第一遍（printf 里）执行 protect —— 成功，打印 PASS；
//   第二遍（if 里）又执行 protect —— 包已加密、再次加密失败 → g_fail++。
// 结果就是"21 项全部显示 PASS，但汇总报 2 项失败"这种自相矛盾的现象。
// 见 docs/02-pitfalls.md 坑 27。
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

// ---- 内存管道传输：双向队列模拟网络 ----
class PipeTransport : public DtlsTransport {
public:
    // 把对方的方向队列拿来当输出
    void link(PipeTransport* peer) { peer_ = peer; }

    int send(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lk(mutex_);
        for (size_t i = 0; i < len; i++) tx_.push_back(data[i]);
        return int(len);
    }

    int recv(uint8_t* buf, size_t cap, int timeout_ms) override {
        const int64_t deadline = now_ms() + timeout_ms;
        while (now_ms() <= deadline) {
            {
                std::lock_guard<std::mutex> lk(peer_->mutex_);
                if (!peer_->tx_.empty()) {
                    size_t n = std::min(cap, peer_->tx_.size());
                    for (size_t i = 0; i < n; i++) {
                        buf[i] = peer_->tx_.front();
                        peer_->tx_.pop_front();
                    }
                    return int(n);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;   // 超时
    }

private:
    PipeTransport*              peer_ = nullptr;
    std::deque<uint8_t>         tx_;
    std::mutex                  mutex_;
};

// 构造一个合法的最小 RTP 包（12 字节头 + 载荷）
static std::vector<uint8_t> make_rtp(const char* payload, uint16_t seq) {
    std::vector<uint8_t> p(12);
    p[0] = 0x80;                    // V=2
    p[1] = 0x60;                    // PT=96 (dynamic)
    p[2] = uint8_t(seq >> 8);
    p[3] = uint8_t(seq);
    p[4] = 0x00; p[5] = 0x00; p[6] = 0x0F; p[7] = 0xA0;   // timestamp
    p[8] = 0x11; p[9] = 0x22; p[10] = 0x33; p[11] = 0x44; // SSRC
    for (const char* q = payload; *q; q++) p.push_back(uint8_t(*q));
    return p;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ DTLS-SRTP 测试 ################\n\n");

    PipeTransport ta, tb;
    ta.link(&tb);
    tb.link(&ta);

    DtlsSrtpSession a, b;

    // ---- 握手必须在两个线程里并发进行（DTLS 是双向握手）----
    std::atomic<bool> a_ok{false}, b_ok{false};
    std::string a_fp, b_fp;

    printf("=== 1. DTLS 握手（双方并发）===\n");
    std::thread th_a([&] {
        DtlsSrtpSession::Config ca;
        ca.is_client = true;          // A 作 DTLS client（对应 SDP a=setup:active）
        ca.handshake_timeout_ms = 15000;
        a_ok = a.handshake(&ta, ca);
    });
    std::thread th_b([&] {
        DtlsSrtpSession::Config cb;
        cb.is_client = false;         // B 作 DTLS server
        cb.handshake_timeout_ms = 15000;
        b_ok = b.handshake(&tb, cb);
    });
    th_a.join();
    th_b.join();

    CHECK(a_ok.load(), "A 侧 DTLS 握手成功");
    CHECK(b_ok.load(), "B 侧 DTLS 握手成功");
    CHECK(a.ready() && b.ready(), "双方 SRTP 会话就绪");

    a_fp = a.local_fingerprint();
    b_fp = b.local_fingerprint();
    printf("    A 本地指纹: %s\n", a_fp.c_str());
    printf("    B 本地指纹: %s\n", b_fp.c_str());
    CHECK(!a_fp.empty() && !b_fp.empty(), "双方都产出了证书指纹");

    // ---- 指纹交叉校验 ----
    // ★ 重要：DTLS 握手在两侧"完成"的时刻并不对称。
    //   client（A）在收到 server 的 Finished 后完成 —— 此时它已拿到对端证书；
    //   server（B）在自己发出 Finished 后就完成 —— 此时对端的最后一批报文
    //   （含 Certificate）可能还在路上。
    //   因此 server 侧需要稍等并再驱动一轮，才能读到对端证书。
    // 这是实测观察到的行为（B 侧 SSL_get_peer_certificate 为 NULL），
    // 属于 DTLS 协议特性而非缺陷。见 docs/02-pitfalls.md 坑 26。
    printf("\n=== 2. 证书指纹一致性（防 MITM 的基础）===\n");
    for (int i = 0; i < 100 && b.peer_fingerprint().empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        b.refresh_peer_fingerprint();
    }
    a.refresh_peer_fingerprint();
    printf("    A.peer_fingerprint() = %s\n",
           a.peer_fingerprint().empty() ? "(空)" : a.peer_fingerprint().c_str());
    printf("    B.peer_fingerprint() = %s\n",
           b.peer_fingerprint().empty() ? "(空)" : b.peer_fingerprint().c_str());
    CHECK(a.peer_fingerprint() == b_fp, "A 看到的对端指纹 == B 的本地指纹");
    CHECK(b.peer_fingerprint() == a_fp, "B 看到的对端指纹 == A 的本地指纹");

    // ---- 密钥派生一致性 ----
    printf("\n=== 3. RFC 5764 密钥派生 ===\n");
    CHECK(a.client_key() == b.client_key(), "双方导出的 client_key 一致");
    CHECK(a.server_key() == b.server_key(), "双方导出的 server_key 一致");
    CHECK(a.client_salt() == b.client_salt(), "双方导出的 client_salt 一致");
    CHECK(a.server_salt() == b.server_salt(), "双方导出的 server_salt 一致");
    CHECK(!a.client_key().empty() && a.client_key().size() == 16, "client_key 长度 16 字节");
    CHECK(!a.client_salt().empty() && a.client_salt().size() == 14, "client_salt 长度 14 字节");

    // ---- SRTP 加解密往返 ----
    printf("\n=== 4. SRTP 加密 / 解密往返 ===\n");
    const char* secret = "HELLO_RTC_AUDIO_PAYLOAD";
    auto rtp = make_rtp(secret, 1);
    auto plain = rtp;

    int len = int(rtp.size());
    const int plain_len = len;

    CHECK(a.protect(rtp.data(), &len), "A 加密成功");
    printf("    明文 %d 字节 -> 加密后 %d 字节（增加 %d 字节认证标签）\n",
           plain_len, len, len - plain_len);
    CHECK(len > plain_len, "加密后长度增加（SRTP 认证标签）");
    CHECK(memcmp(rtp.data(), plain.data(), plain_len) != 0, "密文与明文不同（确实加密了）");

    int dlen = len;
    CHECK(b.unprotect(rtp.data(), &dlen), "B 解密成功");
    CHECK(dlen == plain_len, "解密后长度还原");
    CHECK(memcmp(rtp.data(), plain.data(), plain_len) == 0, "解密后内容与明文完全一致");

    // ---- 防篡改 ----
    printf("\n=== 5. 防篡改（SRTP 认证保护）===\n");
    auto rtp2 = make_rtp("TAMPER_TEST", 2);
    int len2 = int(rtp2.size());
    const int plain2_len = len2;
    const bool enc_ok = a.protect(rtp2.data(), &len2);
    CHECK(enc_ok, "加密成功（用于篡改测试的包）");
    if (enc_ok) {
        // 篡改载荷中的一个字节：解密端应因 HMAC 校验失败而拒绝
        rtp2[13] ^= 0xFF;
        int dlen2 = len2;
        const bool tamper_rejected = !b.unprotect(rtp2.data(), &dlen2);
        CHECK(tamper_rejected, "篡改后的密文被解密端拒绝（HMAC-SHA1 认证生效）");
    }

    // ---- 未加密的明文包也应被拒绝（防止绕过加密）----
    printf("\n=== 6. 明文包被拒绝（认证覆盖完整性）===\n");
    {
        auto rtp3 = make_rtp("NOT_PROTECTED", 3);
        int len3 = int(rtp3.size());
        const bool rejected = !b.unprotect(rtp3.data(), &len3);
        CHECK(rejected, "未加密的明文包被 SRTP 拒绝");
    }

    // ---- 统计 ----
    auto sa = a.stats();
    auto sb = b.stats();
    printf("\n=== 统计 ===\n");
    printf("    A: 加密 %llu 包, 失败 %llu\n",
           (unsigned long long)sa.protected_pkts, (unsigned long long)sa.protect_failed);
    printf("    B: 解密 %llu 包, 失败 %llu\n",
           (unsigned long long)sb.unprotected_pkts, (unsigned long long)sb.unprotect_failed);

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
