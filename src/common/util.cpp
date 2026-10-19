#include "common/util.h"

#include <cstdio>
#include <random>

namespace rtc {

namespace {
std::mt19937& rng() {
    static thread_local std::mt19937 g(std::random_device{}());
    return g;
}
}  // namespace

uint32_t rand_u32() {
    static thread_local std::uniform_int_distribution<uint32_t> d(0, 0xFFFFFFFFu);
    return d(rng());
}
uint16_t rand_u16() {
    static thread_local std::uniform_int_distribution<uint32_t> d(0, 0xFFFFu);
    return static_cast<uint16_t>(d(rng()));
}

bool read_pcm_s16le(const std::string& path, std::vector<int16_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    out->resize(static_cast<size_t>(sz) / sizeof(int16_t));
    size_t rd = fread(out->data(), 1, static_cast<size_t>(sz), f);
    fclose(f);
    out->resize(rd / sizeof(int16_t));
    return true;
}

bool write_pcm_s16le(const std::string& path, const int16_t* data, size_t samples) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t wr = fwrite(data, sizeof(int16_t), samples, f);
    fclose(f);
    return wr == samples;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
    // 标准 CRC-32（IEEE 802.3），查表法
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        inited = true;
    }
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace rtc
