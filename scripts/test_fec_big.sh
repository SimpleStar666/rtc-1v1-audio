#!/usr/bin/env bash
# 验证 FEC 在大载荷（>255 字节）下是否正确
cat > /tmp/fecbig.cpp <<'EOF'
#include <cstdio>
#include <vector>
#include <cstring>
#include "net/loss_protection.h"
using namespace rtc;

int main() {
    // 48kbps × 60ms = 360 字节/包，超过 u8 上限 255
    const size_t PAYLOAD = 360;
    printf("=== FEC 大载荷测试（每包 %zu 字节，> 255）===\n", PAYLOAD);

    for (size_t sz : {size_t(120), size_t(360), size_t(900)}) {
        FecEncoder::Config ecfg;
        ecfg.group_size = 4;
        FecEncoder enc(ecfg);
        FecDecoder dec;

        std::vector<std::vector<uint8_t>> src;
        for (uint16_t i = 0; i < 4; i++) {
            std::vector<uint8_t> p(sz);
            for (size_t b = 0; b < sz; b++) p[b] = uint8_t((i * 37 + b) & 0xFF);
            src.push_back(p);
        }

        std::vector<uint8_t> fec_pkt;
        bool got = false;
        for (uint16_t i = 0; i < 4; i++)
            got = enc.add_source(i, src[i].data(), sz, &fec_pkt) || got;
        if (!got) { printf("  %4zu 字节: ✗ 未生成 FEC\n", sz); continue; }

        // 丢掉第 2 个包，其余收到
        dec.on_source(0, src[0].data(), sz);
        dec.on_source(1, src[1].data(), sz);
        dec.on_source(3, src[3].data(), sz);

        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> rec;
        dec.on_fec(fec_pkt.data(), fec_pkt.size(), &rec);

        bool ok = rec.size() == 1 && rec[0].first == 2 &&
                  rec[0].second.size() == sz &&
                  memcmp(rec[0].second.data(), src[2].data(), sz) == 0;
        printf("  %4zu 字节/包 (FEC包 %zu 字节): %s\n",
               sz, fec_pkt.size(), ok ? "✅ 恢复成功且内容一致" : "❌ 恢复失败");
        if (!ok) return 1;
    }
    printf("\n结论：载荷超过 255 字节时 FEC 仍能正确恢复（u8→u16 修复生效）\n");
    return 0;
}
EOF
cd /home/ts/rtc-1v1-audio
g++ -std=c++17 -I src /tmp/fecbig.cpp -o /tmp/fecbig \
  build/librtc_net.a build/librtc_common.a -lpthread 2>&1 | head -5
/tmp/fecbig
