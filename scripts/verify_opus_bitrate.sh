#!/bin/bash
# 快速验证 Opus 是否遵守运行时设定码率
cat > /tmp/opbr.c <<'EOF'
#include <opus/opus.h>
#include <stdio.h>
#include <math.h>
int main(){
    int err;
    OpusEncoder* e = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY_REQUEST, 0);
    opus_encoder_ctl(e, OPUS_SET_VBR_REQUEST, 0);   // CBR

    for (int target = 16000; target <= 48000; target += 16000) {
        opus_encoder_ctl(e, OPUS_SET_BITRATE_REQUEST, target);
        long total = 0; int frames = 0;
        int16_t pcm[320];
        unsigned char out[1500];
        double phase = 0, step = 2*M_PI*440.0/16000.0;
        for (int f = 0; f < 250; f++) {          // 5 秒
            for (int i = 0; i < 320; i++) { pcm[i] = (int16_t)(8000*sin(phase)); phase += step; if (phase > 2*M_PI) phase -= 2*M_PI; }
            int n = opus_encode(e, pcm, 320, out, sizeof(out));
            if (n > 0) { total += n; frames++; }
        }
        // 250 帧 × 20ms = 5 秒
        printf("  target=%5d bps -> 实测 %5ld bps (%d 帧, %ld 字节)\n",
               target, total*8*1000/5000, frames, total);
    }
    opus_encoder_destroy(e);
    return 0;
}
EOF
gcc /tmp/opbr.c -o /tmp/opbr $(pkg-config --cflags --libs opus) -lm 2>&1 | head -3
echo "=== Opus 运行时码率验证（CBR, complexity=0, 440Hz 正弦）==="
timeout 30 /tmp/opbr
