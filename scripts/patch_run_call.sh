#!/usr/bin/env bash
# 用 Python 把 run.sh 的 do_call 替换成真正的 --call 模式
python3 - <<'PYEOF'
import re, io

path = '/home/ts/rtc-1v1-audio/scripts/run.sh'
src = open(path, encoding='utf-8').read()

new_call = r'''# ---------- 双端真实通话（真正的音频互通） ----------
#
# 这是本项目最主要的用法：两个进程各自采集音频，
# 经 Opus 编码 -> 打包 -> FEC -> DTLS-SRTP 加密 -> ICE(UDP) 传输，
# 对端解密 -> 抖动缓冲 -> 解码 -> 播放。
#
# 两端采集**不同**的音频源，这样能直接用耳朵/频谱验证"听到的是对方的"：
#   A 采扫频（200->3000Hz），B 采嘟嘟（150Hz）
#   => A 的录音里应是 150Hz，B 的录音里应是扫频
do_call() {
  do_build || return 1
  title "双端实时通话（完整媒体链路）"
  echo -e "  ${C_D}链路: 采集 -> Opus编码 -> 打包 -> FEC -> DTLS-SRTP加密 -> ICE(UDP)${C_0}"
  echo -e "  ${C_D}      -> 对端: ICE收到 -> SRTP解密 -> 抖动缓冲 -> 解码 -> 播放${C_0}"
  echo ""

  # 准备两种明显不同的音频源，便于验证"听到的是对方"
  bash "$ROOT/scripts/make_audio.sh" "$AUDIO_DIR" > /dev/null 2>&1 || true
  local src_a="$AUDIO_DIR/sweep.pcm"   # A 发：扫频
  local src_b="$AUDIO_DIR/sine440.pcm" # B 发：单音 440Hz
  [ -f "$src_a" ] || src_a=""
  [ -f "$src_b" ] || src_b=""

  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.4
  cd "$BUILD"

  echo -e "  ${C_D}[1/4] 启动信令服务器 (端口 $PORT)${C_0}"
  ./signal_server --port $PORT > /tmp/rtc_srv.log 2>&1 &
  local SRV=$!
  sleep 1
  if grep -q "server started" /tmp/rtc_srv.log; then ok "服务器就绪"
  else err "服务器未就绪"; cat /tmp/rtc_srv.log; return 1; fi

  local secs=8
  echo -e "  ${C_D}[2/4] 端点 A（主叫，采集=扫频 200->3000Hz）${C_0}"
  local A_ARGS=(--call --signal "ws://127.0.0.1:$PORT" --room demo --name A
                --seconds $secs --out /tmp/call_A.pcm)
  [ -n "$src_a" ] && A_ARGS+=(--audio-file "$src_a")
  timeout $((secs+25)) ./peer "${A_ARGS[@]}" > /tmp/rtc_A.log 2>&1 &
  local PA=$!
  sleep 1.5

  echo -e "  ${C_D}[3/4] 端点 B（被叫，采集=单音 440Hz）${C_0}"
  local B_ARGS=(--call --signal "ws://127.0.0.1:$PORT" --room demo --name B
                --seconds $secs --out /tmp/call_B.pcm)
  [ -n "$src_b" ] && B_ARGS+=(--audio-file "$src_b")
  timeout $((secs+25)) ./peer "${B_ARGS[@]}" > /tmp/rtc_B.log 2>&1 &
  local PB=$!

  # 等它们跑完
  wait $PA 2>/dev/null; local RA=$?
  wait $PB 2>/dev/null; local RB=$?
  kill $SRV 2>/dev/null

  echo -e "  ${C_D}[4/4] 收集结果${C_0}"
  echo ""

  # ---- 展示建立过程（截取关键阶段）----
  title "A 端（主叫）建链过程"
  grep -E "^--- 阶段|^  ✓|^  \[角色\]|^  \[信令\] 已入房|已应用|对端指纹校验|^    \[" /tmp/rtc_A.log \
    | head -24 | sed 's/^/    /'
  echo ""
  title "B 端（被叫）建链过程"
  grep -E "^--- 阶段|^  ✓|^  \[角色\]|^  \[信令\] 已入房|已应用|对端指纹校验|^    \[" /tmp/rtc_B.log \
    | head -24 | sed 's/^/    /'

  echo ""
  title "通话结果"
  for side in A B; do
    local f=/tmp/rtc_${side}.log
    printf "  %s 端: " "$side"
    local sent recv srtp
    sent=$(grep "发送包数" "$f" | grep -oE '[0-9]+' | head -1)
    recv=$(grep "接收包数" "$f" | grep -oE '[0-9]+' | head -1)
    srtp=$(grep "SRTP 解密" "$f" | grep -oE '[0-9]+ 包（失败 [0-9]+）' | head -1)
    printf "发送 %-4s 包 / 接收 %-4s 包 / SRTP 解密 %s\n" "${sent:-?}" "${recv:-?}" "${srtp:-?}"
  done

  echo ""
  if grep -q "✅ 通话成功" /tmp/rtc_A.log && grep -q "✅ 通话成功" /tmp/rtc_B.log; then
    ok "两端通话都成功（A=$RA B=$RB）"
  else
    err "通话未完全成功（A=$RA B=$RB），详见日志"
  fi

  # ---- 验证"听到的是对方" ----
  title "验证：听到的是对方的声音（不是自环）"
  python3 - "$AUDIO_DIR" <<'PYEOF2'
import wave, struct, math, sys, os

def pcm_to_wav(src, dst, sr=16000):
    if not os.path.exists(src): return False
    raw = open(src,'rb').read()
    with wave.open(dst,'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(raw)
    return True

def dominant(pcm, sr=16000):
    start = min(len(pcm)//4, sr)
    seg = pcm[start:start+sr]
    if len(seg) < sr//2: return None
    best_f, best_e = 0, 0
    for f in [150, 200, 300, 440, 620, 880, 1320, 1800, 2400, 3000]:
        k = 2*math.cos(2*math.pi*f/sr)
        s1 = s2 = 0.0
        for x in seg:
            s0 = x + k*s1 - s2
            s2, s1 = s1, s0
        e = s1*s1 + s2*s2 - k*s1*s2
        if e > best_e: best_e, best_f = e, f
    return best_f

def load(p):
    w = wave.open(p,'rb'); d = w.readframes(w.getnframes()); w.close()
    return [struct.unpack('<h', d[i:i+2])[0] for i in range(0,len(d),2)]

print(f"    A 采集源: 扫频 200->3000Hz   B 采集源: 单音 440Hz")
for side, src_desc, expect in [('A','440Hz 单音(B的)', 440), ('B','扫频(A的)', None)]:
    pcm_p = f'/tmp/call_{side}.pcm'
    wav_p = f'/tmp/call_{side}.wav'
    if not pcm_to_wav(pcm_p, wav_p):
        print(f"    {side} 端: 无录音文件"); continue
    pcm = load(wav_p)
    if not pcm: print(f"    {side} 端: 空录音"); continue
    f = dominant(pcm)
    amp = max(abs(x) for x in pcm)
    print(f"    {side} 端录音: 主导频率 = {f} Hz  (期望听到 {src_desc}), 最大幅度 = {amp}")
PYEOF2

  echo ""
  warn "两端录音: /tmp/call_A.wav  /tmp/call_B.wav"
  ok "试听: paplay /tmp/call_A.wav   （或双击 Windows 里的 wav）"
  echo -e "  ${C_D}提示: 若需要反复演示，先跑 ./scripts/run.sh audio 生成测试音频${C_0}"
}
'''

# 用新函数替换旧的 do_call（从注释头到下一个 "# ---------- 弱网演示"）
pattern = re.compile(
    r'# -+ 双端真实通话 -+\n.*?(?=# -+ 弱网演示 -+)',
    re.S)
if not pattern.search(src):
    print("❌ 没找到 do_call 区块")
    sys.exit(1)

out = pattern.sub(new_call + '\n', src)
open(path, 'w', encoding='utf-8').write(out)
print("✅ do_call 已替换为真正的 --call 模式")
PYEOF