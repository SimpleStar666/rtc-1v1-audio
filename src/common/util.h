#pragma once
// 通用小工具：字节序、随机数、时间戳格式化、统计
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace rtc {

// ---- 大端读写（RTP 风格网络字节序）----
inline void put_u16_be(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put_u32_be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline uint16_t get_u16_be(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t get_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// ---- 小端读写（本地存储/JSON 友好）----
inline void put_u16_le(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint16_t get_u16_le(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get_u32_le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// ---- 随机数 ----
uint32_t rand_u32();
uint16_t rand_u16();

// ---- 16bit 小端 PCM 文件 IO ----
bool read_pcm_s16le(const std::string& path, std::vector<int16_t>* out);
bool write_pcm_s16le(const std::string& path, const int16_t* data, size_t samples);

// ---- 简单统计 ----
struct Stats {
    size_t   count = 0;
    double   sum = 0;
    double   sum_sq = 0;
    int64_t  min_v = INT64_MAX;
    int64_t  max_v = INT64_MIN;

    void add(double v) {
        count++;
        sum += v;
        sum_sq += v * v;
        if (v < min_v) min_v = static_cast<int64_t>(v);
        if (v > max_v) max_v = static_cast<int64_t>(v);
    }
    double mean() const { return count ? sum / count : 0; }
    double stddev() const {
        if (count < 2) return 0;
        double m = mean();
        return sqrt((sum_sq / count) - m * m);
    }
};

// ---- CRC32（用于 FEC / 包完整性校验）----
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

}  // namespace rtc
