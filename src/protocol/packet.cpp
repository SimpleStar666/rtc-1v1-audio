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

uint16_t Packet::header_crc(const PacketHeader& h) {
    // 只覆盖"头部的关键字段"，避免把 CRC 自身算进去
    uint8_t tmp[14];
    put_u16_be(tmp + 0, h.session_id);
    put_u16_be(tmp + 2, h.seq);
    put_u32_be(tmp + 4, h.timestamp);
    tmp[8]  = static_cast<uint8_t>(h.phase);
    tmp[9]  = h.flags;
    put_u16_be(tmp + 10, h.trans_id);
    put_u16_be(tmp + 12, h.payload_len);

    uint16_t sum = 0;
    for (size_t i = 0; i < sizeof(tmp); i++) {
        sum = uint16_t((sum << 1) | (sum >> 15));  // 循环左移 1
        sum ^= tmp[i];
    }
    return uint16_t(~sum);
}

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

    put_u16_be(out + 0, h.session_id);
    put_u16_be(out + 2, h.seq);
    put_u32_be(out + 4, h.timestamp);
    out[8] = static_cast<uint8_t>(h.phase);
    out[9] = h.flags;
    put_u16_be(out + 10, h.trans_id);
    put_u16_be(out + 12, h.payload_len);
    put_u16_be(out + 14, header_crc(h));

    if (len && payload) memcpy(out + PacketHeader::kSize, payload, len);
    return static_cast<int>(total);
}

bool Packet::parse(const uint8_t* buf, size_t len, PacketHeader* h, std::vector<uint8_t>* payload) {
    if (!buf || len < PacketHeader::kSize) {
        LOGE(kTag, "packet too short: %zu < %zu", len, PacketHeader::kSize);
        return false;
    }

    h->session_id  = get_u16_be(buf + 0);
    h->seq         = get_u16_be(buf + 2);
    h->timestamp   = get_u32_be(buf + 4);
    h->phase       = static_cast<Phase>(buf[8]);
    h->flags       = buf[9];
    h->trans_id    = get_u16_be(buf + 10);
    h->payload_len = get_u16_be(buf + 12);
    const uint16_t crc_recv = get_u16_be(buf + 14);

    // 包头校验：早期漏掉这一步，导致损坏包被当成正常音频播放，产生噪声
    const uint16_t crc_calc = header_crc(*h);
    if (crc_recv != crc_calc) {
        LOGW(kTag, "header crc mismatch: recv=0x%04x calc=0x%04x (seq=%u)",
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
