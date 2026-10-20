#!/usr/bin/env bash
# 决定性验证：证明两端真的在互相听
# A 采集"扫频"，B 采集"滴滴声"。若真的互通：
#   A 的播放文件里应该是"滴滴声"（B 的），B 的里应该是"扫频"（A 的）
cat > /tmp/proof.sh <<'EOS'
#!/usr/bin/env bash
set -u
cd /home/ts/rtc-1v1-audio/build
export PATH="$HOME/.local/bin:$PATH"

# 准备两种明显不同的音频
python3 - <<'PY'
import wave, struct, math, os
os.makedirs('/tmp/rtc_demo', exist_ok=True)
sr = 16000
def save(name, samples):
    raw = b''.join(struct.pack('<h', max(-32768,min(32767,int(v)))) for v in samples)
    open(f'/tmp/rtc_demo/{name}.pcm','wb').write(raw)
    with wave.open(f'/tmp/rtc_demo/{name}.wav','wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(raw)
# 扫频 200->3000Hz
n = sr*8
save('sweep_proof', [9000*math.sin(2*math.pi*(200+(3000-200)*i/n)*i/sr) for i in range(n)])
# 低频方波状"嘟嘟" 150Hz
n2 = sr*8
save('buzz_proof', [9000*math.sin(2*math.pi*150*i/sr) for i in range(n2)])
print("音频已生成")
PY

pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5
./signal_server --port 8899 > /tmp/p_srv.log 2>&1 &
SRV=$!; sleep 1

# A 采扫频
timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
  --seconds 6 --audio-file /tmp/rtc_demo/sweep_proof.pcm \
  --out /tmp/proof_A.pcm > /tmp/p_A.log 2>&1 &
PA=$!
sleep 1.5
# B 采嘟嘟
timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
  --seconds 6 --audio-file /tmp/rtc_demo/buzz_proof.pcm \
  --out /tmp/proof_B.pcm > /tmp/p_B.log 2>&1 &
PB=$!
wait $PA 2>/dev/null; RA=$?
wait $PB 2>/dev/null; RB=$?
kill $SRV 2>/dev/null

echo ""
echo "退出码: A=$RA B=$RB"
echo ""
echo "--- A 的结果 ---"
grep -E "✅|❌" /tmp/p_A.log
echo "--- B 的结果 ---"
grep -E "✅|❌" /tmp/p_B.log
EOS
chmod +x /tmp/proof.sh; chown ts:ts /tmp/proof.sh
sudo -u ts bash /tmp/proof.sh

echo ""
echo "=== 频谱分析：谁的录音里是谁的声音？ ==="
python3 - <<'PY'
import wave, struct, math, os

def read_wav(p):
    w = wave.open(p, 'rb')
    d = w.readframes(w.getnframes())
    w.close()
    return [struct.unpack('<h', d[i:i+2])[0] for i in range(0, len(d), 2)]

def dominant_freq(pcm, sr=16000):
    """用 Goertzel 在几个候选频率上测能量，找出主导频率"""
    # 取中间一段（跳过开头的静默/建立期）
    start = min(len(pcm)//4, sr)
    seg = pcm[start:start+sr]  # 1 秒
    if len(seg) < sr//2: return None, 0
    best_f, best_e = 0, 0
    for f in [150, 200, 400, 800, 1500, 2500, 3000]:
        k = 2*math.cos(2*math.pi*f/sr)
        s1 = s2 = 0.0
        for x in seg:
            s0 = x + k*s1 - s2
            s2, s1 = s1, s0
        e = s1*s1 + s2*s2 - k*s1*s2
        if e > best_e: best_e, best_f = e, f
    return best_f, best_e

# 把 PCM 转 WAV 便于分析
for n in ['A','B']:
    src = f'/tmp/proof_{n}.pcm'
    dst = f'/tmp/proof_{n}.wav'
    if os.path.exists(src):
        raw = open(src,'rb').read()
        with wave.open(dst,'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(raw)

for n in ['A','B']:
    p = f'/tmp/proof_{n}.wav'
    if not os.path.exists(p): 
        print(f"  {n}: 文件不存在"); continue
    pcm = read_wav(p)
    f, e = dominant_freq(pcm)
    amp = max(abs(x) for x in pcm) if pcm else 0
    label = {150:'150Hz(嘟嘟, 来自 B)', 200:'扫频起点200Hz', 3000:'扫频高频'}.get(f, f'{f}Hz')
    print(f"  {n} 端录音: 主导频率={f}Hz ({label}), 最大幅度={amp}")
PY
