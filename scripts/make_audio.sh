#!/usr/bin/env bash
# 生成各种测试音频（PCM + WAV），用于 --audio-file 与试听
# 用法: bash scripts/make_audio.sh [输出目录]
set -e
OUT="${1:-/tmp/rtc_demo}"
mkdir -p "$OUT"

python3 - "$OUT" <<'PYEOF'
import sys, wave, struct, math, os
out = sys.argv[1]

def save(name, samples, sr=16000):
    """既存裸 PCM（供 --audio-file），也存 WAV（供试听/分发）"""
    raw = b''.join(struct.pack('<h', max(-32768, min(32767, int(v)))) for v in samples)
    pcm = os.path.join(out, name + '.pcm')
    wav = os.path.join(out, name + '.wav')
    open(pcm, 'wb').write(raw)
    with wave.open(wav, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(raw)
    print(f"  {name:16s} {len(samples):7d} 样本  {len(raw):8d} 字节  ({len(raw)/sr/2:.2f}s)")

sr = 16000

# 1) 纯正弦 440Hz（最基础，能听出"单音"）
n = int(sr * 2)
save('sine440', [8000*math.sin(2*math.pi*440*i/sr) for i in range(n)], sr)

# 2) 双音和弦（440 + 880，能听出"和声"）
save('chord', [6000*math.sin(2*math.pi*440*i/sr) + 3000*math.sin(2*math.pi*880*i/sr)
               for i in range(n)], sr)

# 3) 扫频（200Hz → 3000Hz，能明显听出频率变化——最适合验证"音频真的通了"）
n2 = int(sr * 3)
f0, f1 = 200.0, 3000.0
save('sweep', [9000*math.sin(2*math.pi*(f0 + (f1-f0)*i/n2)*i/sr) for i in range(n2)], sr)

# 4) 莫尔斯风格的"滴滴"声（听感接近人耳可辨的提示音）
n3 = int(sr * 4)
samples = []
for i in range(n3):
    t = i / sr
    # 每 0.5 秒一个 0.3 秒的音，频率随时间变化
    cycle = int(t / 0.5) % 4
    on = (t % 0.5) < 0.3
    freq = [440, 620, 880, 1320][cycle]
    samples.append(9000*math.sin(2*math.pi*freq*t) if on else 0.0)
save('beeps', samples, sr)

# 5) 白噪声（听感是"沙沙"，用于测抖动缓冲/PLC 的效果）
import random
random.seed(42)
noise = []
for i in range(int(sr*3)):
    # 加窗避免削波
    noise.append(random.gauss(0, 3000))
save('noise', noise, sr)

# 6) 静音（全 0，用于测试 DTX / PLC）
save('silence', [0.0]*int(sr*1), sr)

print(f"\n  输出目录: {out}")
print(f"  裸 PCM 用于 --audio-file，WAV 用于试听")
PYEOF

echo ""
echo "=== 生成的 WAV 列表 ==="
ls -la "$OUT"/*.wav | awk '{printf "  %-40s %8d 字节\n", $9, $5}'
echo ""
echo "=== 试听方法 ==="
echo "  WSL 内:   paplay $OUT/sweep.wav"
echo "  Windows:  直接双击 $OUT\\sweep.wav"
