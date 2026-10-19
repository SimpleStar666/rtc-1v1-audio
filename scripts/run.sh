#!/usr/bin/env bash
# ============================================================
#  rtc-1v1-audio 一键运行入口
#
#  用法:
#    ./scripts/run.sh              # 交互式菜单（推荐，看这个就够）
#    ./scripts/run.sh demo         # 一键跑完整演示
#    ./scripts/run.sh build        # 只编译
#    ./scripts/run.sh test         # 跑全部测试
#    ./scripts/run.sh loopback     # 本地自环（听得到声音）
#    ./scripts/run.sh call         # 双端真实通话（信令配对）
#    ./scripts/run.sh weaknet      # 弱网矩阵演示
#    ./scripts/run.sh audio        # 生成测试音频
#    ./scripts/run.sh help         # 帮助
# ============================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="$ROOT/build"
AUDIO_DIR="/tmp/rtc_demo"
PORT=8899

C_B="\033[1m"; C_G="\033[32m"; C_Y="\033[33m"; C_R="\033[31m"; C_D="\033[2m"; C_0="\033[0m"
ok()   { echo -e "  ${C_G}✓${C_0} $*"; }
warn() { echo -e "  ${C_Y}!${C_0} $*"; }
err()  { echo -e "  ${C_R}✗${C_0} $*"; }
title(){ echo -e "\n${C_B}=== $* ===${C_0}"; }

# ---------- 环境自检 ----------
check_env() {
  local bad=0
  for t in cmake g++ pkg-config python3; do
    command -v $t >/dev/null 2>&1 || { err "缺少 $t"; bad=1; }
  done
  export PATH="$HOME/.local/bin:$PATH"   # pip 安装的 cmake
  command -v cmake >/dev/null 2>&1 || { err "cmake 不可用（尝试: pip install --user 'cmake>=3.28'）"; bad=1; }
  for l in opus libsrtp2 nice libwebsockets glib-2.0 openssl; do
    pkg-config --exists $l 2>/dev/null || { err "缺少开发库: $l"; bad=1; }
  done
  return $bad
}

# ---------- 编译 ----------
#
# 权限与工具链自检（都是实际踩过的坑）：
#
#  坑 A（权限）：
#    如果 build/ 或仓库目录不属于当前用户（典型原因：之前用 sudo/root 编译过），
#    那么 `mkdir -p build` 会**静默成功**（目录已存在），
#    但接着 `> build.log` 就报 "Permission denied"，
#    真正的错误信息还被重定向吞掉了，用户只看到莫名其妙的一行。
#
#  坑 B（cmake 版本）：
#    Ubuntu 20.04 自带的 cmake 是 3.16，而本项目要求 >= 3.21。
#    如果新版 cmake 只装在别人的 home 里（比如 root 的 ~/.local），
#    普通用户用不到，会报 "CMake 3.21 or higher is required"。
#
# 本函数在动手之前就检测这两件事，并给出可直接复制执行的修复命令。
# 见 docs/02-pitfalls.md 坑 35 / 坑 36。
check_cmake_version() {
  local min_major=3 min_minor=21
  if ! command -v cmake >/dev/null 2>&1; then
    err "找不到 cmake"
    echo "      修复: python3 -m pip install --user \"cmake>=3.28\""
    return 1
  fi
  local ver maj rest minor
  ver=$(cmake --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  maj=${ver%%.*}
  rest=${ver#*.}
  minor=${rest%%.*}
  if [ -z "$ver" ]; then
    warn "无法解析 cmake 版本，继续尝试"
    return 0
  fi
  if [ "${maj:-0}" -gt "$min_major" ] || \
     { [ "${maj:-0}" -eq "$min_major" ] && [ "${minor:-0}" -ge "$min_minor" ]; }; then
    return 0
  fi
  err "cmake 版本过低：$ver（需要 >= ${min_major}.${min_minor}）"
  echo "      cmake 路径: $(command -v cmake)"
  echo ""
  echo -e "      ${C_B}修复方法${C_0}"
  echo "        python3 -m pip install --user \"cmake>=3.28\""
  echo "        export PATH=\"\$HOME/.local/bin:\$PATH\""
  echo ""
  echo -e "      ${C_D}说明：Ubuntu 20.04 自带的 cmake 是 3.16，不满足本项目要求。${C_0}"
  echo -e "      ${C_D}      装好后 ~/.local/bin/cmake 才是新版，必须让它排在 PATH 前面。${C_0}"
  return 1
}

check_writable() {
  local dir="$1" label="$2"
  # 目录不存在时检查其父目录
  if [ ! -d "$dir" ]; then
    dir=$(dirname "$dir")
  fi
  if [ ! -w "$dir" ]; then
    err "$label 不可写：$dir"
    local owner
    owner=$(ls -ld "$dir" 2>/dev/null | awk '{print $3":"$4}')
    echo "      所有者: ${owner:-未知}    当前用户: $(id -un)"
    echo ""
    echo -e "      ${C_B}修复方法（任选其一）${C_0}"
    echo "        sudo rm -rf build"
    echo "        sudo chown -R $(id -un):$(id -gn) ."
    echo ""
    echo -e "      ${C_D}原因：通常是因为之前用 sudo 或 root 身份编译过，${C_0}"
    echo -e "      ${C_D}      留下的文件属于 root，普通用户无法覆盖。${C_0}"
    return 1
  fi
  return 0
}

do_build() {
  title "编译"
  # 新版 cmake 通常在 ~/.local/bin，优先用它
  export PATH="$HOME/.local/bin:$PATH"

  # ---- 自检 1：cmake 版本 ----
  check_cmake_version || return 1

  # ---- 自检 2：目录权限 ----
  check_writable "$ROOT" "仓库目录" || return 1
  if [ -e "$BUILD" ]; then
    check_writable "$BUILD" "build 目录" || return 1
    # 目录可写但旧日志文件可能属于其他用户 → 尝试能否截断
    if [ -e "$BUILD/build.log" ] && ! ( : > "$BUILD/build.log" ) 2>/dev/null; then
      err "build/build.log 不可写（可能属于其他用户）"
      echo "      修复：sudo rm -rf build"
      return 1
    fi
  fi

  mkdir -p "$BUILD" 2>/dev/null
  if [ ! -d "$BUILD" ]; then
    err "无法创建 build 目录：$BUILD"
    return 1
  fi
  cd "$BUILD" || return 1

  # ---- 配置 ----
  if [ ! -f CMakeCache.txt ]; then
    echo -e "  ${C_D}cmake $(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')  首次配置...${C_0}"
    if ! cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
         > cmake.log 2>&1; then
      err "cmake 配置失败，日志：build/cmake.log"
      tail -20 cmake.log | sed 's/^/      /'
      # 配置失败会留下半成品缓存，下次会报
      # "could not find CMAKE_PROJECT_NAME in Cache" —— 清掉，避免误导
      rm -f CMakeCache.txt
      echo -e "      ${C_D}已清理失败的 CMakeCache.txt（避免影响下次配置）${C_0}"
      return 1
    fi
  fi

  # ---- 编译 ----
  echo -e "  ${C_D}编译中（-j$(nproc)）...${C_0}"
  if ! cmake --build . -j"$(nproc)" > build.log 2>&1; then
    err "编译失败，日志：build/build.log"
    grep -E 'error:|Error' build.log | head -12 | sed 's/^/      /'
    return 1
  fi

  ok "编译成功（产物在 build/）"
  ls -1 selftest weaknet_test peer signal_server ice_test dtls_srtp_test \
        loss_protection_test metrics_test e2e_test 2>/dev/null | sed 's/^/      /'
}

# ---------- 测试 ----------
do_test() {
  do_build || return 1
  title "全部测试"
  cd "$BUILD"
  local fail=0
  run() {
    local name="$1"; shift
    printf "  %-26s" "$name"
    local out rc
    out=$(timeout 120 "$@" 2>&1); rc=$?
    local last
    last=$(echo "$out" | grep -E "结果：" | tail -1 | sed 's/^#* *//')
    if [ -n "$last" ]; then
      # 有汇总行的（selftest / ice / dtls / loss / metrics / e2e）
      if echo "$last" | grep -q "全部通过"; then
        printf '%b\n' "${C_G}${last}${C_0}"
      else
        printf '%b\n' "${C_R}${last}${C_0}"
        fail=$((fail+1))
      fi
    else
      # 无汇总行的（weaknet / loopback）：用退出码判断
      if [ $rc -eq 0 ]; then
        printf '%b\n' "${C_G}✓ 通过（组件自打印结果，无汇总行）${C_0}"
      else
        printf '%b\n' "${C_R}✗ 失败（退出码 $rc）${C_0}"
        fail=$((fail+1))
      fi
    fi
  }
  run "媒体层自检"      ./selftest
  run "弱网矩阵"        ./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 8
  run "本地自环"        ./peer --loopback --seconds 2
  run "ICE 穿透(+Trickle)" ./ice_test --seconds 1
  run "DTLS-SRTP"       ./dtls_srtp_test
  run "弱网对抗"        ./loss_protection_test
  run "指标统计"        ./metrics_test
  run "端到端(无丢包)"  ./e2e_test --seconds 2
  run "端到端(10%丢包)" ./e2e_test --loss 0.10 --seconds 3
  echo ""
  [ $fail -eq 0 ] && ok "全部通过" || err "有 $fail 项未通过"
  return $fail
}

# ---------- 生成音频 ----------
do_audio() {
  title "生成测试音频"
  bash "$ROOT/scripts/make_audio.sh" "$AUDIO_DIR"
  ok "裸 PCM 可用于 --audio-file；WAV 可直接试听"
  if command -v paplay >/dev/null 2>&1; then
    ok "试听命令: paplay $AUDIO_DIR/sweep.wav"
  fi
}

# ---------- 本地自环（能听到声音） ----------
do_loopback() {
  do_build || return 1
  title "本地自环验证（媒体链路端到端，不经网络）"
  echo -e "  ${C_D}说明: 采集 -> Opus 编码 -> 打包 -> 回灌 -> 抖动缓冲 -> 解码 -> 落盘${C_0}"
  echo ""
  cd "$BUILD"
  local src=""
  [ -f "$AUDIO_DIR/sweep.pcm" ] && src="$AUDIO_DIR/sweep.pcm"
  local secs=3
  local args=(--loopback --seconds $secs --out /tmp/peer_out.pcm)
  [ -n "$src" ] && args+=(--audio-file "$src")
  timeout 60 ./peer "${args[@]}" 2>&1 | tail -20

  echo ""
  title "试听结果"
  python3 "$AUDIO_DIR/pcm2wav.py" /tmp/peer_out.pcm /tmp/peer_out.wav 16000 2>/dev/null \
    || python3 - <<'PY'
import wave
raw = open('/tmp/peer_out.pcm','rb').read()
with wave.open('/tmp/peer_out.wav','wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(raw)
print("OK: /tmp/peer_out.pcm -> /tmp/peer_out.wav")
PY
  if command -v paplay >/dev/null 2>&1; then
    warn "即将在 WSL 播放解码后的音频（WSLg 会转发到 Windows 扬声器）..."
    timeout 20 paplay /tmp/peer_out.wav && ok "播放完成——你应该听到了扫频声" \
      || err "播放失败（可改用 Windows 播放器打开 /tmp/peer_out.wav）"
  else
    warn "无 paplay，请手动播放 /tmp/peer_out.wav"
  fi
  ok "播放落盘文件: /tmp/peer_out.wav（同时生成了 .pcm）"
}

# ---------- 双端真实通话 ----------
do_call() {
  do_build || return 1
  title "双端真实通话（信令配对 + 媒体）"
  echo -e "  ${C_D}流程: join -> 配对 -> offer/answer -> 候选交换 -> 媒体传输${C_0}"
  echo ""

  pkill -f signal_server 2>/dev/null; sleep 0.3
  cd "$BUILD"

  echo -e "  ${C_D}[1/3] 启动信令服务器 (端口 $PORT)${C_0}"
  ./signal_server --port $PORT > /tmp/rtc_srv.log 2>&1 &
  local SRV=$!
  sleep 1
  grep -q "server started" /tmp/rtc_srv.log && ok "服务器就绪" || { err "服务器未就绪"; cat /tmp/rtc_srv.log; return 1; }

  local secs=6
  local src=""
  [ -f "$AUDIO_DIR/sweep.pcm" ] && src="$AUDIO_DIR/sweep.pcm"

  echo -e "  ${C_D}[2/3] 启动端点 A（主叫，$secs 秒）${C_0}"
  local A_ARGS=(--signal "ws://127.0.0.1:$PORT" --room demo --name A --peer-test
                --seconds $secs --out /tmp/peer_A.pcm)
  [ -n "$src" ] && A_ARGS+=(--audio-file "$src")
  timeout $((secs+12)) ./peer "${A_ARGS[@]}" > /tmp/rtc_A.log 2>&1 &
  local PA=$!
  sleep 1

  echo -e "  ${C_D}[3/3] 启动端点 B（被叫，$secs 秒）${C_0}"
  local B_ARGS=(--signal "ws://127.0.0.1:$PORT" --room demo --name B --peer-test
                --seconds $secs --out /tmp/peer_B.pcm)
  [ -n "$src" ] && B_ARGS+=(--audio-file "$src")
  timeout $((secs+12)) ./peer "${B_ARGS[@]}" > /tmp/rtc_B.log 2>&1 &
  local PB=$!

  wait $PA 2>/dev/null; wait $PB 2>/dev/null
  kill $SRV 2>/dev/null

  echo ""
  title "端点 A（主叫）"
  # 只取带 [A] 前缀的过程行（避免与下面"信令面结果"行重复匹配）
  grep -E "^[[:space:]]*\[A\]" /tmp/rtc_A.log | sed 's/^/    /'
  grep -E "信令面结果" /tmp/rtc_A.log | tail -1 | sed 's/^/    /'
  echo ""
  title "端点 B（被叫）"
  grep -E "^[[:space:]]*\[B\]" /tmp/rtc_B.log | sed 's/^/    /'
  grep -E "信令面结果" /tmp/rtc_B.log | tail -1 | sed 's/^/    /'
  echo ""
  title "服务器转发记录"
  grep -E "joined|paired|forwarded" /tmp/rtc_srv.log | sed 's/^/    /'

  echo ""
  title "完成"
  ok "两端媒体落盘: /tmp/peer_A.pcm, /tmp/peer_B.pcm"
  warn "日志: /tmp/rtc_srv.log /tmp/rtc_A.log /tmp/rtc_B.log"
}

# ---------- 弱网演示 ----------
do_weaknet() {
  do_build || return 1
  title "弱网矩阵（抖动缓冲 + NACK/FEC + 拥塞控制）"
  cd "$BUILD"
  echo -e "  ${C_D}对照组：丢包率从 0% 递增，观察\"检测丢包\"是否与实际吻合${C_0}"
  echo ""
  printf "  %-8s %-10s %-10s %-10s %-9s\n" "丢包率" "实际丢失" "检测丢包" "播放帧数" "PLC补偿"
  printf "  %s\n" "$(printf '%.0s-' {1..48})"
  for L in 0 2 5 8 15; do
    local out
    out=$(timeout 60 ./weaknet_test --loss $L --jitter 20 --delay 50 --seconds 8 2>/dev/null)
    # 用 grep -o 只取第一个数字，避免 awk 把 "(7.5%)" 里的数字也吞进去
    local lost det played plc
    lost=$(echo   "$out" | grep "丢包帧数"     | grep -oE '[0-9]+' | head -1)
    det=$(echo    "$out" | grep "检测丢包"     | grep -oE '[0-9]+' | head -1)
    played=$(echo "$out" | grep "播放帧数"     | grep -oE '[0-9]+' | head -1)
    plc=$(echo    "$out" | grep "PLC 补偿次数" | grep -oE '[0-9]+' | head -1)
    # 检测丢包 == 实际丢失 时标绿
    local mark=""
    [ "${lost:-x}" = "${det:-y}" ] && mark="${C_G}✓${C_0}" || mark="${C_R}✗${C_0}"
    printf "  %-8s %-10s %-10s %-10s %-9s %b\n" "${L}%" "$lost" "$det" "$played" "$plc" "$mark"
  done
  echo ""
  echo -e "  ${C_D}最后一列的 ✓ 表示\"检测丢包 == 实际丢失\"，这是统计口径正确的直接证据${C_0}"
  echo ""
  title "端到端弱网（含 SRTP 加密 + ICE 通道）"
  printf "  %-10s %-10s %-10s %-12s %s\n" "丢包率" "实际丢弃" "检测丢包" "FEC恢复" "SRTP 加/解"
  printf "  %s\n" "$(printf '%.0s-' {1..52})"
  for L in 0 0.10 0.25; do
    local out
    out=$(timeout 90 ./e2e_test --loss "$L" --seconds 3 2>/dev/null)
    local loss det fec srtp
    loss=$(echo "$out" | grep "模拟丢弃(源)" | grep -oE '[0-9]+' | head -1)
    det=$(echo  "$out" | grep "^  检测丢包"     | grep -oE '[0-9]+' | head -1)
    fec=$(echo  "$out" | grep "FEC 恢复"        | grep -oE '[0-9]+' | head -1)
    srtp=$(echo "$out" | grep "SRTP 加密/解密" | grep -oE '[0-9]+ / [0-9]+' | head -1)
    printf "  %-10s %-10s %-10s %-12s %s\n" "$L" "${loss:--}" "${det:--}" "${fec:--}" "${srtp:--}"
  done
  echo ""
  echo -e "  ${C_D}注: --loss 是小数概率，每轮实际丢弃数有随机波动，看\"检测丢包\"是否与\"实际丢弃\"吻合${C_0}"
}

# ---------- 完整演示 ----------
do_demo() {
  title "完整演示"
  check_env || echo -e "  ${C_Y}环境有缺失项，仍尝试继续${C_0}"
  do_audio
  do_test || return 1
  do_loopback
  do_call
  echo ""
  title "演示结束"
  ok "详细数据见 docs/03-verification.md"
  ok "踩坑记录见 docs/02-pitfalls.md"
}

# ---------- 帮助 ----------
# 注意：不要用 $(echo -e "...") 做嵌套引用 —— bash 的引号配对会出错。
# 用变量 + printf 分别输出，简单可靠。
HRULE="────────────────────────────────────────────────────────"
do_help() {
  echo ""
  printf '%b\n' "${C_B}rtc-1v1-audio —— 1v1 实时语音通话（自建 WebRTC 音频栈）${C_0}"
  echo ""
  printf '%b\n' "${C_B}用法${C_0}"
  echo "  ./scripts/run.sh             交互式菜单（推荐）"
  echo "  ./scripts/run.sh <命令>"
  echo ""
  printf '%b\n' "${C_B}命令${C_0}"
  printf '  %b%-10s%b 完整演示（编译+测试+自环+通话）\n'   "$C_Y" "demo"     "$C_0"
  printf '  %b%-10s%b 只编译\n'                             "$C_Y" "build"    "$C_0"
  printf '  %b%-10s%b 跑全部 9 项测试并汇总\n'              "$C_Y" "test"     "$C_0"
  printf '  %b%-10s%b 本地自环 —— %b能听到解码后的声音%b\n' "$C_Y" "loopback" "$C_0" "$C_G" "$C_0"
  printf '  %b%-10s%b 双端真实通话（起服务器 + A/B 配对）\n' "$C_Y" "call"    "$C_0"
  printf '  %b%-10s%b 弱网矩阵（丢包 0/2/5/8/15%%）\n'      "$C_Y" "weaknet"  "$C_0"
  printf '  %b%-10s%b 生成测试音频（PCM 用于 --audio-file，WAV 用于试听）\n' "$C_Y" "audio" "$C_0"
  printf '  %b%-10s%b 环境自检\n'                           "$C_Y" "env"      "$C_0"
  printf '  %b%-10s%b 本帮助\n'                             "$C_Y" "help"     "$C_0"
  echo ""
  printf '%b\n' "${C_B}单独运行各组件${C_0}（编译后，在 build/ 下）"
  echo "  ./selftest                                      媒体层 6 组自检"
  echo "  ./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10"
  echo "  ./peer --loopback --seconds 3                    本地自环"
  echo "  ./ice_test --seconds 3                          ICE 穿透 + Trickle ICE"
  echo "  ./ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2"
  echo "  ./dtls_srtp_test                                DTLS-SRTP（21 项）"
  echo "  ./loss_protection_test                          NACK/FEC/拥塞控制"
  echo "  ./metrics_test                                  指标统计"
  echo "  ./e2e_test --seconds 4                          完整链路（加密+ICE）"
  echo "  ./e2e_test --loss 0.25 --seconds 4              弱网下的完整链路"
  echo ""
  printf '%b\n' "${C_B}听声音${C_0}"
  echo "  WSL 内:   paplay /tmp/rtc_demo/sweep.wav"
  echo "  Windows:  双击 /tmp/rtc_demo/*.wav（或桌面 rtc-demo-audio 目录）"
  echo ""
  printf '%b\n' "${C_B}文档${C_0}"
  echo "  docs/00-architecture.md   架构与 WebRTC 分层映射"
  echo "  docs/01-progress-log.md   开发进度与设计决策"
  echo "  docs/02-pitfalls.md       33 个真实踩坑记录"
  echo "  docs/03-verification.md   全部实测数据与复现步骤"
  echo "  docs/04-how-to-run.md     本手册（怎么跑、怎么玩）"
  echo ""
}

menu() {
  do_help
  echo -e "${C_B}请选择要执行的操作:${C_0}"
  select choice in "完整演示" "编译" "全部测试" "本地自环（听声音）" \
                   "双端通话" "弱网矩阵" "生成音频" "环境自检" "退出"; do
    case "$choice" in
      "完整演示")      do_demo ;;
      "编译")          do_build ;;
      "全部测试")      do_test ;;
      "本地自环（听声音）") do_loopback ;;
      "双端通话")      do_call ;;
      "弱网矩阵")      do_weaknet ;;
      "生成音频")      do_audio ;;
      "环境自检")      check_env && ok "环境正常" || err "环境有缺失" ;;
      "退出")          echo "bye"; break ;;
      *)               echo "无效选择" ;;
    esac
    echo ""
    echo -e "${C_D}按回车继续...${C_0}"; read -r _
  done
}

case "${1:-menu}" in
  demo)     do_demo ;;
  build)    do_build ;;
  test)     do_test ;;
  loopback) do_loopback ;;
  call)     do_call ;;
  weaknet)  do_weaknet ;;
  audio)    do_audio ;;
  env)      check_env && ok "环境正常（可编译运行）" || err "环境有缺失" ;;
  help|-h|--help) do_help ;;
  menu|"")  menu ;;
  *) err "未知命令: $1"; do_help; exit 1 ;;
esac
