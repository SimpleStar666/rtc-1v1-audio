#!/bin/bash
# 验证"能听到声音"的路径
echo "=== 1. 检查 WSL 音频服务（paplay 需要 PulseAudio）==="
ls -la /mnt/wslg/ 2>/dev/null | head -5
echo "PULSE_SERVER=$PULSE_SERVER"
[ -S "$PULSE_SERVER" ] 2>/dev/null && echo "  ✅ PulseAudio socket 存在" || echo "  ✗ 无 PulseAudio（无法在 WSL 内直接播放）"

echo ""
echo "=== 2. 用 Python 把 PCM 转成 WAV（Windows 可直接播放）==="
python3 - <<'PYEOF'
import wave, struct, os, math

# 先造一段真实的 440Hz+880Hz 双音 PCM 用于演示（600ms）
sr = 16000
dur = 0.6
n = int(sr * dur)
data = bytearray()
for i in range(n):
    t = i / sr
    v = int(6000 * math.sin(2*math.pi*440*t) + 3000 * math.sin(2*math.pi*880*t))
    data += struct.pack('<h', max(-32768, min(32767, v)))

os.makedirs('/tmp/rtc_demo', exist_ok=True)
with wave.open('/tmp/rtc_demo/demo.wav', 'wb') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(bytes(data))
print(f"  ✅ 生成 /tmp/rtc_demo/demo.wav（{n} 样本, {len(data)} 字节）")

# 验证：把刚生成的 PCM 也转成 WAV（证明转换逻辑可用）
pcm = '/tmp/peer_out.pcm'
if os.path.exists(pcm):
    raw = open(pcm, 'rb').read()
    with wave.open('/tmp/rtc_demo/peer_out.wav', 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(raw)
    print(f"  ✅ 转换 {pcm} -> /tmp/rtc_demo/peer_out.wav（{len(raw)} 字节）")

# 简便的通用转换脚本
open('/tmp/rtc_demo/pcm2wav.py', 'w').write('''#!/usr/bin/env python3
"""把裸 PCM（16kHz/单声道/16bit LE）转成 WAV。
用法: python3 pcm2wav.py <输入.pcm> [输出.wav] [采样率]
"""
import wave, sys
src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.wav'
sr  = int(sys.argv[3]) if len(sys.argv) > 3 else 16000
with wave.open(dst, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(open(src, 'rb').read())
print(f"OK: {src} -> {dst} ({sr}Hz/mono/16bit)")
''')
print("  ✅ 生成转换脚本 /tmp/rtc_demo/pcm2wav.py")
PYEOF

echo ""
echo "=== 3. 测试转换脚本 ==="
python3 /tmp/rtc_demo/pcm2wav.py /tmp/peer_out.pcm /tmp/rtc_demo/test.wav 16000

echo ""
echo "=== 4. 产物 ==="
ls -la /tmp/rtc_demo/

echo ""
echo "=== 5. 复制到 Windows 桌面可播放的位置 ==="
DEST="/mnt/c/Users/小陶/Desktop/rtc-demo-audio"
mkdir -p "$DEST"
cp /tmp/rtc_demo/*.wav "$DEST/" 2>/dev/null
cp /tmp/rtc_demo/pcm2wav.py "$DEST/" 2>/dev/null
ls -la "$DEST/" 2>/dev/null && echo "  ✅ 已复制到 Windows 桌面: $DEST"
