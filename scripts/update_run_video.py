#!/usr/bin/env python3
# 更新 run.sh：加入视频测试、视频通话模式、视频说明
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/scripts/run.sh'
s = open(P, encoding='utf-8').read()
n = 0
def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# 1) 测试列表加入视频
rep('''  run "DTLS-SRTP"       ./dtls_srtp_test''',
'''  run "DTLS-SRTP"       ./dtls_srtp_test
  run "VP8 视频编解码"  ./vp8_test
  run "视频管线"        ./video_test''',
'测试列表加视频')

# 2) do_call 里的 peer 命令加视频参数
rep('''  local A_ARGS=(--call --signal "ws://127.0.0.1:$PORT" --room demo --name A
                --seconds $secs --out /tmp/call_A.pcm)''',
'''  local A_ARGS=(--call --signal "ws://127.0.0.1:$PORT" --room demo --name A
                --seconds $secs --out /tmp/call_A.pcm
                --video-size 320x240 --video-fps 15 --video-kbps 400
                --video-source test:bars --video-sink /tmp/vsink_A)''',
'A 端加视频参数')

rep('''  local B_ARGS=(--call --signal "ws://127.0.0.1:$PORT" --room demo --name B
                --seconds $secs --out /tmp/call_B.pcm)''',
'''  local B_ARGS=(--call --signal "ws://127.0.0.1:$PORT" --room demo --name B
                --seconds $secs --out /tmp/call_B.pcm
                --video-size 320x240 --video-fps 15 --video-kbps 400
                --video-source test:motion --video-sink /tmp/vsink_B)''',
'B 端加视频参数')

# 3) do_call 开始前清理视频落盘目录
rep('''  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.4''',
'''  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.4
  rm -rf /tmp/vsink_A /tmp/vsink_B; mkdir -p /tmp/vsink_A /tmp/vsink_B''',
'清理视频落盘目录')

# 4) do_call 的结果展示加视频
rep('''  echo ""
  if grep -q "✅ 通话成功" /tmp/rtc_A.log && grep -q "✅ 通话成功" /tmp/rtc_B.log; then''',
'''  # ---- 视频统计 ----
  echo ""
  title "视频传输"
  for side in A B; do
    local vf=/tmp/rtc_${side}.log
    printf "  %s 端: " "$side"
    local venc vdec vdrop vkbps
    venc=$(grep "采集/编码帧" "$vf" | grep -oE '[0-9]+ / [0-9]+' | head -1)
    vdec=$(grep "解码/丢弃帧" "$vf" | grep -oE '[0-9]+ / [0-9]+' | head -1)
    vkbps=$(grep "发送分片" "$vf" | grep -oE '实测 [0-9]+ kbps' | head -1)
    printf "编码 %-12s 解码/丢弃 %-12s %s\\n" "${venc:-?}" "${vdec:-?}" "${vkbps:-}"
  done
  # 解码帧落盘数量
  local na nb
  na=$(ls /tmp/vsink_A/*.ppm 2>/dev/null | wc -l)
  nb=$(ls /tmp/vsink_B/*.ppm 2>/dev/null | wc -l)
  echo "  解码帧落盘: A 端收到 $na 帧 / B 端收到 $nb 帧（PPM）"
  if [ "$na" -gt 0 ] && [ "$nb" -gt 0 ]; then
    ok "视频双向传输成功（可用图片查看器打开 /tmp/vsink_*/*.ppm）"
  fi

  echo ""
  if grep -q "✅ 通话成功" /tmp/rtc_A.log && grep -q "✅ 通话成功" /tmp/rtc_B.log; then''',
'do_call 展示视频统计')

# 5) 帮助里加视频说明
rep('''  printf '  %b%-10s%b 双端真实通话（起服务器 + A/B 配对）\\n' "$C_Y" "call"    "$C_0"''',
'''  printf '  %b%-10s%b 双端音视频通话（起服务器 + A/B 配对，含 VP8 视频）\\n' "$C_Y" "call" "$C_0"''',
'帮助 call 说明')

# 6) 单独运行列表加视频测试
rep('''  echo "  ./dtls_srtp_test                                DTLS-SRTP（21 项）"''',
'''  echo "  ./dtls_srtp_test                                DTLS-SRTP（21 项）"
  echo "  ./vp8_test                                      VP8 编解码（PSNR/关键帧/码率）"
  echo "  ./video_test                                    视频管线（分片/重组/PLI）"''',
'帮助加视频测试')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ run.sh 共 {n} 处')
