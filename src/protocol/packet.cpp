#include "protocol/packet.h"

#include <cstring>

#include "common/logging.h"
#include "common/util.h"

namespace rtc {

namespace {
constexpr const char* kTag = "packet";
}

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::kStart: return "START";
        case Phase::kBody:  return "BODY";
        case Phase::kEnd:   return "END";
        case Phase::kBreak: return "BREAK";
        case Phase::kClose: return "CLOSE";
        case Phase::kPing:  return "PING";
        case Phase::kPong:  return "PONG";
        case Phase::kStats: return "STATS";
    }
    return "UNKNOWN";
}

// 包头校验：覆盖 CRC 字段之前的**全部** 18 字节。
//
// 为什么不只覆盖自定义扩展段：
//   本包格式既用于"独立传输"（loopback 自环、进程内测试），
//   也用于"SRTP 加密传输"。在独立传输时，RTP 兼容段没有 SRTP 的 HMAC 保护，
//   如果不纳入校验，损坏版本位/序号/时间戳的包会被误判为合法包。
//   回归测试（selftest「损坏包头被 CRC 拒绝」）正是抓到了这个问题：
//   最初只覆盖 8 字节扩展段时，篡改第 2 字节（seq）无法被检出。
//   见 docs/02-pitfalls.md 坑 30。
uint16_t Packet::ext_crc(const PacketHeader& h) {
    uint8_t tmp[18];
    // RTP 兼容段
    tmp[0] = kRtpVersionByte;
    tmp[1] = uint8_t((h.marker ? 0x80 : 0x00) | (kRtpPayloadType & 0x7F));
    put_u16_be(tmp + 2, h.seq);
    put_u32_be(tmp + 4, h.timestamp);
    put_u32_be(tmp + 8, uint32_t(h.session_id));
    // 自定义扩展段
    tmp[12] = static_cast<uint8_t>(h.phase);
    tmp[13] = h.flags;
    put_u16_be(tmp + 14, h.trans_id);
    put_u16_be(tmp + 16, h.payload_len);

    uint16_t sum = 0;
    for (size_t i = 0; i < sizeof(tmp); i++) {
        sum = uint16_t((sum << 1) | (sum >> 15));   // 循环左移 1
        sum ^= tmp[i];
    }
    return uint16_t(~sum);
}

// 兼容旧接口名（头文件里叫 header_crc）
uint16_t Packet::header_crc(const PacketHeader& h) { return ext_crc(h); }

int Packet::serialize(const PacketHeader& h_in, const uint8_t* payload, size_t len,
                      uint8_t* out, size_t out_cap) {
    if (len > 0xFFFF) {
        LOGE(kTag, "payload too large: %zu", len);
        return -1;
    }
    const size_t total = PacketHeader::kSize + len;
    if (total > out_cap) {
        LOGE(kTag, "serialize overflow: need=%zu cap=%zu", total, out_cap);
        return -1;
    }

    PacketHeader h = h_in;
    h.payload_len = static_cast<uint16_t>(len);

    // ---- RTP 兼容段（12 字节，网络字节序）----
    out[0] = kRtpVersionByte;
    out[1] = uint8_t((h.marker ? 0x80 : 0x00) | (kRtpPayloadType & 0x7F));
    put_u16_be(out + 2, h.seq);
    put_u32_be(out + 4, h.timestamp);
    // session_id 映射到 SSRC：高 16 位保留，低 16 位放 session_id
    put_u32_be(out + 8, uint32_t(h.session_id));

    // ---- 自定义扩展段（8 字节）----
    out[12] = static_cast<uint8_t>(h.phase);
    out[13] = h.flags;
    put_u16_be(out + 14, h.trans_id);
    put_u16_be(out + 16, h.payload_len);
    put_u16_be(out + 18, ext_crc(h));

    if (len && payload) memcpy(out + PacketHeader::kSize, payload, len);
    return static_cast<int>(total);
}

bool Packet::parse(const uint8_t* buf, size_t len, PacketHeader* h, std::vector<uint8_t>* payload) {
    if (!buf || len < PacketHeader::kSize) {
        LOGE(kTag, "packet too short: %zu < %zu", len, PacketHeader::kSize);
        return false;
    }

    // ---- 校验 RTP 兼容段 ----
    const uint8_t version = (buf[0] >> 6) & 0x03;
    if (version != 2) {
        // 版本位不对，说明这不是我们的包（或包已损坏）
        LOGW(kTag, "bad RTP version: %u (first byte=0x%02x)", version, buf[0]);
        return false;
    }

    h->marker      = (buf[1] & 0x80) != 0;
    h->seq         = get_u16_be(buf + 2);
    h->timestamp   = get_u32_be(buf + 4);
    h->session_id  = uint16_t(get_u32_be(buf + 8));
    h->phase       = static_cast<Phase>(buf[12]);
    h->flags       = buf[13];
    h->trans_id    = get_u16_be(buf + 14);
    h->payload_len = get_u16_be(buf + 16);
    const uint16_t crc_recv = get_u16_be(buf + 18);

    // 扩展段校验：早期漏掉这一步，损坏包会被当成正常音频解码，产生噪声
    const uint16_t crc_calc = ext_crc(*h);
    if (crc_recv != crc_calc) {
        LOGW(kTag, "ext crc mismatch: recv=0x%04x calc=0x%04x (seq=%u)",
             crc_recv, crc_calc, h->seq);
        return false;
    }

    if (len < PacketHeader::kSize + h->payload_len) {
        LOGE(kTag, "payload truncated: have=%zu need=%zu",
             len - PacketHeader::kSize, size_t(h->payload_len));
        return false;
    }

    payload->assign(buf + PacketHeader::kSize,
                    buf + PacketHeader::kSize + h->payload_len);
    return true;
}

}  // namespace rtc
