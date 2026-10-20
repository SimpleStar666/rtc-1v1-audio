#include "crypto/dtls_srtp.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <srtp2/srtp.h>

#include <cstring>

#include "common/logging.h"
#include "common/util.h"   // now_ms()

namespace rtc {

namespace {
constexpr const char* kTag = "dtls_srtp";

// RFC 5764 §4.2 —— DTLS-SRTP 的密钥导出 label
// 导出 60 字节：client_key(16) + server_key(16) + client_salt(14) + server_salt(14)
constexpr const char* kSrtpExporterLabel = "EXTRACTOR-dtls_srtp";
constexpr size_t kKeyLen    = 16;
constexpr size_t kSaltLen   = 14;
constexpr size_t kExportLen = (kKeyLen + kSaltLen) * 2;   // 60

// SRTP 加密套件：AES-128-ICM + HMAC-SHA1-80（WebRTC 最常用）
constexpr const char* kSrtpProfile = "SRTP_AES128_CM_SHA1_80";

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 0x0F];
    }
    return s;
}

// 把 "AA:BB:CC" 或 "AABBCC" 转成规范的大写无分隔形式，便于比对
std::string normalize_fp(const std::string& in) {
    std::string s;
    for (char c : in) {
        if (c == ':' || c == ' ' || c == '-') continue;
        s += char(toupper(uint8_t(c)));
    }
    return s;
}
}  // namespace

DtlsSrtpSession::DtlsSrtpSession() {
    // libsrtp 全局初始化（幂等）
    static bool srtp_inited = false;
    if (!srtp_inited) {
        if (srtp_init() != srtp_err_status_ok) {
            LOGE(kTag, "srtp_init failed");
        } else {
            srtp_inited = true;
        }
    }
}

DtlsSrtpSession::~DtlsSrtpSession() {
    if (srtp_in_)  { srtp_dealloc(srtp_in_);  srtp_in_ = nullptr; }
    if (srtp_out_) { srtp_dealloc(srtp_out_); srtp_out_ = nullptr; }
    if (ssl_)  { SSL_free(ssl_); ssl_ = nullptr; }
    if (ctx_)  { SSL_CTX_free(ctx_); ctx_ = nullptr; }
    // rbio_/wbio_ 由 SSL_free 释放
}

// ---------------- 自签名证书 ----------------
bool DtlsSrtpSession::create_self_signed_cert() {
    // DTLS 用自签名证书即可 —— 身份由"证书指纹"经信令交换来验证，
    // 不依赖 CA 链。这与 WebRTC 的做法一致（WebRTC 不允许 CA 证书）。
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) return false;

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) { EVP_PKEY_free(pkey); return false; }

    bool ok = false;
    do {
        if (EVP_PKEY_keygen_init(pctx) <= 0) break;
        // 2048 位足够（DTLS 握手性能与安全性的平衡点）
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) break;
        if (EVP_PKEY_keygen(pctx, &pkey) <= 0) break;

        X509* x509 = X509_new();
        if (!x509) break;

        // 版本 3、随机序列号、有效期 1 年
        X509_set_version(x509, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 365L * 24 * 3600);

        X509_set_pubkey(x509, pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("rtc-1v1-audio"),
                                   -1, -1, 0);
        X509_set_issuer_name(x509, name);   // 自签名：issuer = subject

        if (!X509_sign(x509, pkey, EVP_sha256())) { X509_free(x509); break; }

        // 放进 SSL_CTX
        if (SSL_CTX_use_certificate(ctx_, x509) != 1) { X509_free(x509); break; }
        if (SSL_CTX_use_PrivateKey(ctx_, pkey) != 1) { X509_free(x509); break; }
        if (SSL_CTX_check_private_key(ctx_) != 1) { X509_free(x509); break; }

        X509_free(x509);
        ok = true;
    } while (false);

    EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string DtlsSrtpSession::compute_fingerprint() const {
    X509* cert = SSL_get_certificate(ssl_);
    if (!cert) return "";

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &mdlen)) return "";

    // 格式化为 "AA:BB:CC:..."（与 SDP a=fingerprint 的格式一致）
    std::string s;
    for (unsigned int i = 0; i < mdlen; i++) {
        if (i) s += ":";
        s += to_hex(md + i, 1);
    }
    return s;
}

// ---------------- 第一阶段：准备（创建证书，可读指纹）----------------
//
// 拆出这一步的目的：真实通话中，双方必须**先交换证书指纹**（写进 offer/answer），
// 然后才能握手。所以指纹必须在握手之前就能拿到。
bool DtlsSrtpSession::prepare(bool is_client, int handshake_timeout_ms) {
    if (ssl_) {
        LOGW(kTag, "already prepared, ignoring");
        return true;
    }
    is_client_ = is_client;
    handshake_timeout_ms_ = handshake_timeout_ms;

    // 1) 创建 SSL_CTX（DTLS，不用 TLS）
    ctx_ = SSL_CTX_new(DTLS_method());
    if (!ctx_) {
        LOGE(kTag, "SSL_CTX_new failed");
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx_, DTLS1_2_VERSION);

    // ★ 证书校验策略：DTLS-SRTP 要求**双方都出示证书**并互相校验指纹。
    //
    // OpenSSL 的 DTLS server 默认**不向 client 索要证书**（不发 CertificateRequest），
    // 因此 server 侧 SSL_get_peer_certificate() 会返回 NULL —— 实测表现为
    // "client 能读到对端指纹、server 读到空"。这不是库的缺陷，而是默认策略。
    //
    // 解决办法：显式开启 SSL_VERIFY_PEER 让 server 请求对端证书，
    // 并挂一个**始终放行**的 verify_callback —— 因为 WebRTC 不用 CA 链，
    // 自签名证书在标准校验下必然失败（unable to get local issuer certificate）。
    // 真正的身份保证来自我们自己的"指纹比对"（见 handshake） 。
    //
    // 注意：不要用 SSL_VERIFY_FAIL_IF_NO_PEER_CERT —— 实测它会直接让握手失败
    // （因为证书无法通过内置校验）。见 docs/02-pitfalls.md 坑 26。
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, [](int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/) -> int {
        // 无条件返回 1：跳过 CA 链校验，指纹由业务层比对
        return 1;
    });

    // DTLS-SRTP 的扩展配置（必须显式启用，否则握手时无法协商 SRTP profile）
    if (SSL_CTX_set_tlsext_use_srtp(ctx_, kSrtpProfile) != 0) {
        LOGE(kTag, "set_tlsext_use_srtp failed");
        return false;
    }

    // 2) 自签名证书
    if (!create_self_signed_cert()) {
        LOGE(kTag, "create_self_signed_cert failed");
        return false;
    }

    // 3) 创建 SSL 对象 + 内存 BIO
    ssl_ = SSL_new(ctx_);
    if (!ssl_) {
        LOGE(kTag, "SSL_new failed");
        return false;
    }
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl_, rbio_, wbio_);   // SSL 接管两个 BIO 的所有权

    if (is_client_) SSL_set_connect_state(ssl_);
    else            SSL_set_accept_state(ssl_);

    // 4) 计算自己的指纹 —— 此时就能读，供信令交换使用
    local_fingerprint_ = compute_fingerprint();
    LOGI(kTag, "prepared (role=%s), local fingerprint: %s",
         is_client_ ? "client/active" : "server/passive",
         local_fingerprint_.c_str());
    return true;
}

// ---------------- 第二阶段：握手 + 校验 + 派生 SRTP 密钥 ----------------
bool DtlsSrtpSession::handshake(DtlsTransport* transport,
                                const std::string& peer_fingerprint) {
    if (!transport) {
        LOGE(kTag, "null transport");
        return false;
    }
    // 兜底：调用方没 prepare 就自动 prepare（用默认角色 client）
    if (!ssl_ && !prepare(is_client_, handshake_timeout_ms_)) return false;

    peer_fingerprint_expected_ = normalize_fp(peer_fingerprint);

    // 5) 握手循环
    if (!do_handshake_loop(transport, handshake_timeout_ms_)) {
        LOGE(kTag, "handshake failed");
        return false;
    }

    // 6) 取出对端证书指纹并校验
    //
    // 注意：这里用 refresh_peer_fingerprint() 读对端证书。
    // 实测发现 client 侧能立即读到、server 侧有时为空 ——
    // 这是 DTLS 握手完成时机在两侧不完全对称导致的（不是库的 bug）。
    // 处理方式：握手完成后再刷新一次；若仍为空则如实记录为"未获取"，
    // 并明确警告（不静默通过）。见 docs/02-pitfalls.md 坑 26。
    refresh_peer_fingerprint();
    LOGI(kTag, "peer fingerprint: %s",
         peer_fingerprint_.empty() ? "(未获取)" : peer_fingerprint_.c_str());

    if (!peer_fingerprint_expected_.empty()) {
        if (peer_fingerprint_.empty()) {
            LOGE(kTag, "cannot verify fingerprint: peer certificate unavailable");
            return false;
        }
        const std::string got = normalize_fp(peer_fingerprint_);
        if (got != peer_fingerprint_expected_) {
            // 指纹不匹配 = 可能被中间人替换证书 → 必须中止
            LOGE(kTag, "FINGERPRINT MISMATCH! expected=%s got=%s (possible MITM)",
                 peer_fingerprint_expected_.c_str(), got.c_str());
            return false;
        }
        LOGI(kTag, "peer fingerprint verified OK");
    } else {
        LOGW(kTag, "no expected fingerprint provided, skipping verification");
    }

    // 7) 导出密钥材料并建立 SRTP 会话
    return export_and_setup_srtp();
}

// ---------------- 一步到位（测试用）----------------
bool DtlsSrtpSession::handshake(DtlsTransport* transport, const Config& cfg) {
    if (!prepare(cfg.is_client, cfg.handshake_timeout_ms)) return false;
    return handshake(transport, cfg.peer_fingerprint);
}


bool DtlsSrtpSession::do_handshake_loop(DtlsTransport* t, int timeout_ms) {
    const int64_t deadline = now_ms() + timeout_ms;
    std::vector<uint8_t> buf(4096);

    while (now_ms() < deadline) {
        // (a) 先收割传输层收到的 DTLS 记录，喂给 rbio。
        //
        // ★ 顺序很重要：必须"先收包"再"推进状态机"。
        // DTLS 是数据报协议，SSL_do_handshake 在缺少对端报文时会返回 WANT_READ；
        // 如果先调它又不及时喂数据，握手就会卡在等待。
        // 见 docs/02-pitfalls.md 坑 26。
        {
            int got = t->recv(buf.data(), buf.size(), 30);
            if (got > 0) {
                BIO_write(rbio_, buf.data(), got);
            } else if (got < 0) {
                LOGE(kTag, "transport recv failed");
                return false;
            }
            // got == 0（超时）不算错误：握手可能正在等对端
        }

        // (b) 推进握手状态机
        int r = SSL_do_handshake(ssl_);
        if (r == 1) {
            // 握手完成：先把剩余的待发记录冲出去
            flush_output(t);

            // ★ 诊断：在"刚完成"这一刻立即检查对端证书是否可见。
            // 实测 client 侧可见、server 侧有时为空，这里加一次立即读取 +
            // 有限次驱动重试，并把结果如实打印，便于定位。
            for (int extra = 0; extra < 5; extra++) {
                if (SSL_get_peer_certificate(ssl_) != nullptr) break;
                int g2 = t->recv(buf.data(), buf.size(), 20);
                if (g2 > 0) BIO_write(rbio_, buf.data(), g2);
                int rr = SSL_do_handshake(ssl_);
                if (rr != 1) break;
                flush_output(t);
            }

            LOGI(kTag, "DTLS handshake completed (cipher=%s)", SSL_get_cipher(ssl_));
            return true;
        }

        int err = SSL_get_error(ssl_, r);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            unsigned long e = ERR_get_error();
            char ebuf[256] = {0};
            if (e) ERR_error_string_n(e, ebuf, sizeof(ebuf));
            LOGE(kTag, "SSL_do_handshake error: %d (%s)", err, ebuf);
            return false;
        }

        // (c) 把 wbio 里待发的 DTLS 记录经传输层发出
        flush_output(t);
    }

    LOGE(kTag, "handshake timeout (%d ms)", timeout_ms);
    return false;
}

// 把 wbio 中累积的 DTLS 记录全部发到传输层。
// 独立成函数以便在"握手完成"和"等待中"两条路径里复用。
void DtlsSrtpSession::flush_output(DtlsTransport* t) {
    int pending;
    while ((pending = BIO_pending(wbio_)) > 0) {
        // 注意：必须用花括号/显式转换，否则
        // `std::vector<uint8_t> out(size_t(pending));` 会被解析成**函数声明**
        // （most vexing parse），报错变成 "request for member 'data' in 'rtc::out'"。
        // 见 docs/02-pitfalls.md 坑 25。
        std::vector<uint8_t> out(static_cast<size_t>(pending));
        int n = BIO_read(wbio_, out.data(), pending);
        if (n <= 0) break;
        int sent = t->send(out.data(), size_t(n));
        if (sent < 0) {
            LOGE(kTag, "transport send failed");
            return;
        }
        LOGD(kTag, "sent DTLS record: %d bytes", n);
    }
}

bool DtlsSrtpSession::export_and_setup_srtp() {
    // RFC 5764：用 DTLS exporter 导出 60 字节密钥材料。
    // client_key(16) | server_key(16) | client_salt(14) | server_salt(14)
    uint8_t material[kExportLen];
    memset(material, 0, sizeof(material));

    if (SSL_export_keying_material(ssl_, material, sizeof(material),
                                   kSrtpExporterLabel, strlen(kSrtpExporterLabel),
                                   nullptr, 0, 0) != 1) {
        LOGE(kTag, "SSL_export_keying_material failed");
        return false;
    }

    client_key_.assign(material, material + kKeyLen);
    server_key_.assign(material + kKeyLen, material + kKeyLen * 2);
    client_salt_.assign(material + kKeyLen * 2, material + kKeyLen * 2 + kSaltLen);
    server_salt_.assign(material + kKeyLen * 2 + kSaltLen, material + kExportLen);

    LOGI(kTag, "SRTP key material exported (client_key=%s...)",
         to_hex(client_key_.data(), 4).c_str());

    // 接收方向 / 发送方向的密钥取决于本端角色：
    //   client 发送用 client_key，接收用 server_key；server 反之。
    //
    // ★ 注意 libsrtp2 的 policy 只需要一个 `key` 指针，且要求
    //   **key 与 salt 是连续存放**的：libsrtp 按套件定义的长度
    //   （AES-128: key 16 字节 + salt 14 字节 = 30 字节）从该指针连续读取。
    //   早期版本用了 `keysalt` 字段 —— 那是不存在的字段（编译报错），
    //   说明我把 key 和 salt 当成两个独立参数了。见 docs/02-pitfalls.md 坑 25。
    // 接收方向 / 发送方向的密钥取决于本端角色：
    //   client 发送用 client_key、接收用 server_key；server 反之。
    // 注意 libsrtp2 的 policy.key 要求 key 与 salt 连续存放。
    const uint8_t* out_key  = is_client_ ? client_key_.data()  : server_key_.data();
    const uint8_t* out_salt = is_client_ ? client_salt_.data() : server_salt_.data();
    const uint8_t* in_key   = is_client_ ? server_key_.data()  : client_key_.data();
    const uint8_t* in_salt  = is_client_ ? server_salt_.data() : client_salt_.data();

    std::vector<uint8_t> out_material, in_material;
    out_material.insert(out_material.end(), out_key, out_key + kKeyLen);
    out_material.insert(out_material.end(), out_salt, out_salt + kSaltLen);
    in_material.insert(in_material.end(), in_key, in_key + kKeyLen);
    in_material.insert(in_material.end(), in_salt, in_salt + kSaltLen);

    // ---- 发送方向（加密）----
    srtp_policy_t out_pol;
    memset(&out_pol, 0, sizeof(out_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtcp);
    out_pol.ssrc.type = ssrc_any_outbound;   // 不限 SSRC
    out_pol.ssrc.value = 0;
    out_pol.key = out_material.data();       // key 与 salt 连续
    out_pol.keys = nullptr;
    out_pol.num_master_keys = 0;
    out_pol.next = nullptr;
    out_pol.window_size = 128;               // 重放窗口
    out_pol.allow_repeat_tx = 0;
    out_pol.ekt = nullptr;
    if (srtp_create(&srtp_out_, &out_pol) != srtp_err_status_ok) {
        LOGE(kTag, "srtp_create (out) failed");
        return false;
    }

    // ---- 接收方向（解密）----
    srtp_policy_t in_pol;
    memset(&in_pol, 0, sizeof(in_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtcp);
    in_pol.ssrc.type = ssrc_any_inbound;     // 不限 SSRC
    in_pol.ssrc.value = 0;
    in_pol.key = in_material.data();
    in_pol.keys = nullptr;
    in_pol.num_master_keys = 0;
    in_pol.next = nullptr;
    in_pol.window_size = 128;
    in_pol.allow_repeat_tx = 0;
    in_pol.ekt = nullptr;
    if (srtp_create(&srtp_in_, &in_pol) != srtp_err_status_ok) {
        LOGE(kTag, "srtp_create (in) failed");
        return false;
    }

    LOGI(kTag, "SRTP sessions ready (profile=%s)", kSrtpProfile);
    return true;
}

// ---------------- SRTP 加解密 ----------------
bool DtlsSrtpSession::protect(uint8_t* rtp, int* len) {
    if (!srtp_out_) return false;

    int out_len = *len;
    srtp_err_status_t st = srtp_protect(srtp_out_, rtp, &out_len);
    if (st != srtp_err_status_ok) {
        stats_.protect_failed++;
        LOGE(kTag, "srtp_protect failed: %d", int(st));
        return false;
    }
    *len = out_len;
    stats_.protected_pkts++;
    return true;
}

bool DtlsSrtpSession::unprotect(uint8_t* srtp, int* len) {
    if (!srtp_in_) return false;

    int out_len = *len;
    srtp_err_status_t st = srtp_unprotect(srtp_in_, srtp, &out_len);
    if (st != srtp_err_status_ok) {
        stats_.unprotect_failed++;
        // 注意：认证失败很可能是"包被篡改或密钥不匹配"，
        // 不是程序错误，所以这里只记统计、不当作致命错误。
        LOGD(kTag, "srtp_unprotect failed: %d", int(st));
        return false;
    }
    *len = out_len;
    stats_.unprotected_pkts++;
    return true;
}

void DtlsSrtpSession::refresh_peer_fingerprint() {
    X509* peer = SSL_get_peer_certificate(ssl_);
    if (!peer) return;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (X509_digest(peer, EVP_sha256(), md, &mdlen)) {
        std::string s;
        for (unsigned int i = 0; i < mdlen; i++) {
            if (i) s += ":";
            s += to_hex(md + i, 1);
        }
        peer_fingerprint_ = s;
    }
    X509_free(peer);
}

}  // namespace rtc
