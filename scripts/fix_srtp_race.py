#!/usr/bin/env python3
# 修两个真实 bug：
#   1) DtlsSrtpSession 的 protect/unprotect 没有线程安全保证
#      —— 音频发送线程 + 视频发送线程并发调 protect，
#         glib 网络线程调 unprotect，libsrtp 的 srtp_t 非线程安全
#   2) 信令服务器不转发 kKeyframeRequest（PLI 到不了对端）

BASE = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio'
H = f'{BASE}/src/crypto/dtls_srtp.h'
C = f'{BASE}/src/crypto/dtls_srtp.cpp'
S = f'{BASE}/src/signal/signal_server.cpp'
n = 0

def patch(path, patches, tag):
    global n
    s = open(path, encoding='utf-8').read()
    for old, new, label in patches:
        if old not in s:
            print(f'  ✗ {label}')
            continue
        s = s.replace(old, new, 1)
        print(f'  ✓ {label}')
        n += 1
    open(path, 'w', encoding='utf-8').write(s)

# ============================================================
# 修复 1：SRTP 加锁
# ============================================================
print('=== 修复 1：SRTP 并发安全 ===')

patch(H, [
    # 引入 mutex
    ('#include <cstdint>\n#include <string>\n#include <vector>',
     '#include <cstdint>\n#include <mutex>\n#include <string>\n#include <vector>',
     'include <mutex>'),
    # 成员
    ("""    srtp_t srtp_in_ = nullptr;    // 解密（接收方向）
    srtp_t srtp_out_ = nullptr;   // 加密（发送方向）""",
     """    srtp_t srtp_in_ = nullptr;    // 解密（接收方向）
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
    mutable std::mutex srtp_in_mutex_;    // 保护 srtp_in_ （解密）""",
     '加两把 SRTP 锁'),
], 'dtls_srtp.h')

patch(C, [
    ("""bool DtlsSrtpSession::protect(uint8_t* rtp, int* len) {
    if (!srtp_out_) return false;

    int out_len = *len;""",
     """bool DtlsSrtpSession::protect(uint8_t* rtp, int* len) {
    if (!srtp_out_) return false;

    // 锁：可能被音频发送线程与视频发送线程并发调用（见头文件说明）
    std::lock_guard<std::mutex> lk(srtp_out_mutex_);

    int out_len = *len;""",
     'protect() 加锁'),
    ("""bool DtlsSrtpSession::unprotect(uint8_t* srtp, int* len) {
    if (!srtp_in_) return false;

    int out_len = *len;""",
     """bool DtlsSrtpSession::unprotect(uint8_t* srtp, int* len) {
    if (!srtp_in_) return false;

    // 锁：在 libnice 的 glib 网络线程调用，与发送侧并发（见头文件说明）
    std::lock_guard<std::mutex> lk(srtp_in_mutex_);

    int out_len = *len;""",
     'unprotect() 加锁'),
], 'dtls_srtp.cpp')

# ============================================================
# 修复 2：服务器转发 kKeyframeRequest
# ============================================================
print()
print('=== 修复 2：服务器转发关键帧请求 ===')

patch(S, [
    ("""        case SignalType::kCandidate:""",
     """        case SignalType::kKeyframeRequest:
            // 视频关键帧请求（对应 RTCP PLI）——必须转发给对端，
            // 否则接收端丢帧后永远等不到新关键帧，画面永久卡死。
            // 这是实测发现的漏洞：服务器 switch 只处理 join/offer/answer/candidate，
            // PLI 被静默丢弃 → 对端"收到 0 次"。见 docs/02-pitfalls.md 坑 46。
            if (s->in_room) handle_forward(wsi, s, m, "keyframe_request");
            break;
        case SignalType::kCandidate:""",
     '服务器转发 kKeyframeRequest'),
], 'signal_server.cpp')

print(f'\n✅ 共 {n} 处修复')