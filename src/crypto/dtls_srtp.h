#pragma once
// DTLS-SRTP 媒体加密 —— 对应 WebRTC 的 DTLS-SRTP（RFC 5764）
//
// 为什么需要两层：
//   SRTP 负责加密媒体载荷，但**不负责密钥协商**。
//   所以要先用 DTLS 握手协商出共享密钥，再从 DTLS 的 master secret
//   派生出 SRTP 的 master key/salt —— 这就是 DTLS-SRTP。
//
// 为什么不直接在 SDP 里写明密钥（SDES）：
//   信令一旦被窃听，密钥就泄露了。DTLS 用证书指纹做端到端认证，能抗中间人。
//   证书指纹通过信令交换（WebRTC 里是 SDP 的 a=fingerprint 行）。
//
// 关键流程：
//   1. DTLS 1.2 握手（自签名证书）——在 ICE 选通的候选对上跑
//   2. 双方各自校验对端证书指纹（防 MITM）
//   3. SSL_export_keying_material(label="EXTRACTOR-dtls_srtp") 导出密钥材料
//   4. 按 RFC 5764 切成 client/server 的 master key + salt
//   5. 用 libsrtp2 创建 SRTP 会话，后续 RTP 走 srtp_protect / srtp_unprotect
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// libsrtp2 与 OpenSSL 的类型直接引入。
//
// 为什么不像其他模块那样做前置声明：
//   libsrtp2 用 `typedef struct srtp_ctx_t_ srtp_ctx_t;` 定义了 srtp_ctx_t，
//   想在自己的头文件里等价前置声明必须复刻 struct tag 名 `srtp_ctx_t_`，
//   很容易与 srtp2 头文件里的定义冲突（实测报 "conflicting declaration"）。
//   这个类本身就持有 srtp_t 成员，依赖是真实的，直接 include 更清晰。
//   （对比见 docs/02-pitfalls.md 坑 21 与坑 25。）
#include <srtp2/srtp.h>

struct ssl_ctx_st;
struct ssl_st;
struct bio_st;

namespace rtc {

// 传输抽象：DTLS 报文经它收发。
// 真实部署时这层是 ICE 选通的候选对（UDP）；测试时可以是本地 socket 对。
class DtlsTransport {
public:
    virtual ~DtlsTransport() = default;
    // 发送一段 DTLS 记录，返回实际发送字节数，<0 表示失败
    virtual int send(const uint8_t* data, size_t len) = 0;
    // 接收一段 DTLS 记录（阻塞至多 timeout_ms），返回字节数，0=超时，<0=错误
    virtual int recv(uint8_t* buf, size_t cap, int timeout_ms) = 0;
};

class DtlsSrtpSession {
public:
    struct Config {
        bool        is_client = true;   // DTLS 角色（active/passive，对应 SDP a=setup）
        int         handshake_timeout_ms = 10000;
        std::string peer_fingerprint;   // 对端证书指纹（16 进制，可空=跳过校验）
        std::string cert_path;          // 空则用自签名证书
        std::string key_path;
    };

    struct Stats {
        uint64_t protected_pkts = 0;
        uint64_t unprotected_pkts = 0;
        uint64_t protect_failed = 0;
        uint64_t unprotect_failed = 0;
    };

    DtlsSrtpSession();
    ~DtlsSrtpSession();

    // ---- 两阶段用法（真实通话必需）----
    //
    // 为什么拆成两步：
    //   信令交换需要**先**拿到自己的证书指纹（写进 offer/answer 发给对端），
    //   然后才能开始握手。如果只在 handshake() 里创建证书，
    //   调用方在交换信令时拿不到指纹，就无法做防 MITM 的指纹校验。
    //
    // 用法：
    //   prepare(is_client)            -> 创建证书/SSL，可读 local_fingerprint()
    //   （经信令交换 sdp，拿到对端指纹）
    //   handshake(transport, peer_fp) -> 做握手 + 校验 + 派生 SRTP 密钥
    bool prepare(bool is_client, int handshake_timeout_ms = 10000);
    bool handshake(DtlsTransport* transport, const std::string& peer_fingerprint);

    // ---- 一步到位（测试用；内部会自动先 prepare）----
    bool handshake(DtlsTransport* transport, const Config& cfg);

    bool prepared() const { return ssl_ != nullptr; }

    // 本地证书指纹（十六进制，用于经信令告知对端）
    std::string local_fingerprint() const { return local_fingerprint_; }

    // 对端证书指纹。
    // 注意：握手刚结束时对端可能还没把证书发完（DTLS 是双向的），
    // 因此这里提供一个"主动重新读取"的接口，供握手完成后调用。
    // 见 docs/02-pitfalls.md 坑 26。
    std::string peer_fingerprint() const { return peer_fingerprint_; }
    void refresh_peer_fingerprint();

    // ---- SRTP 加解密 ----
    // 对 RTP 包加密（就地修改，len 为输入长度，输出为新长度）
    bool protect(uint8_t* rtp, int* len);
    // 对 SRTP 包解密
    bool unprotect(uint8_t* srtp, int* len);

    bool ready() const { return srtp_out_ != nullptr && srtp_in_ != nullptr; }

    // 派生的 SRTP 密钥（仅用于测试比对，生产不应暴露）
    const std::vector<uint8_t>& client_key() const { return client_key_; }
    const std::vector<uint8_t>& server_key() const { return server_key_; }
    const std::vector<uint8_t>& client_salt() const { return client_salt_; }
    const std::vector<uint8_t>& server_salt() const { return server_salt_; }

    const Stats& stats() const { return stats_; }

private:
    bool create_self_signed_cert();
    bool do_handshake_loop(DtlsTransport* t, int timeout_ms);
    void flush_output(DtlsTransport* t);   // 把 wbio 中的 DTLS 记录发出
    bool export_and_setup_srtp();
    std::string compute_fingerprint() const;

    ssl_ctx_st* ctx_ = nullptr;
    ssl_st*     ssl_ = nullptr;
    bio_st*     rbio_ = nullptr;   // 内存 BIO（接收 DTLS 记录）
    bio_st*     wbio_ = nullptr;   // 内存 BIO（待发送的 DTLS 记录）

    srtp_t srtp_in_ = nullptr;    // 解密（接收方向）
    srtp_t srtp_out_ = nullptr;   // 加密（发送方向）

    // ★ libsrtp 的 srtp_t 会话**不是线程安全的**。
    //
    //   本项目里 protect() 会被两个线程调用（音频发送线程 + 视频发送线程），
    //   unprotect() 会被第三个线程调用（libnice 的 glib 网络线程）。
    //   并发进入同一 srtp_t 会破坏其内部重放窗口/序列号状态，
    //   输出**损坏的密文**——接收端解密"成功"但拿到的是垃圾，
    //   表现为解码器报 "Bitstream not supported by this decoder"。
    //
    //   实测：5 轮音视频通话有 2~3 轮 A 端全部视频帧解码失败（间歇性）。
    //   纯音频时只有 1 个发送线程，所以从来没暴露过。
    //   见 docs/02-pitfalls.md 坑 45。
    //
    //   收发方向用的是两个独立的 srtp_t，所以用两把锁，
    //   收发可以并发（不互相阻塞）。
    mutable std::mutex srtp_out_mutex_;   // 保护 srtp_out_（加密）
    mutable std::mutex srtp_in_mutex_;    // 保护 srtp_in_ （解密）

    bool        is_client_ = true;
    int         handshake_timeout_ms_ = 10000;
    std::string local_fingerprint_;
    std::string peer_fingerprint_;
    std::string peer_fingerprint_expected_;

    // 按 RFC 5764 切分出的四段密钥材料
    std::vector<uint8_t> client_key_, server_key_, client_salt_, server_salt_;

    Stats stats_;
};

}  // namespace rtc
