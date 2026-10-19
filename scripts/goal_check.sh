#!/bin/bash
# 目标完成度核对：逐条对照 objective 的要求
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/goalcheck
rm -rf $V

echo "##############################################################"
echo "#  目标完成度核对"
echo "##############################################################"
echo ""

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
cd $V

echo "=== 要求的开源组件是否全部使用 ==="
for lib in opus srtp2 nice libwebsockets openssl; do
  n=$(grep -rl "$lib" src/ CMakeLists.txt 2>/dev/null | wc -l)
  printf "  %-16s 被 %s 个文件使用 %s\n" "$lib" "$n" "$([ "$n" -gt 0 ] && echo ✓ || echo '← 缺')"
done

echo ""
echo "=== 四个部分的代码落地 ==="
declare -A parts=(
  ["① 信令服务"]="src/signal/signal_server.cpp src/signal/signaling_client.cpp"
  ["① Trickle ICE"]="src/ice/ice_transport.cpp"
  ["② STUN/ICE 穿透"]="src/ice/ice_transport.cpp"
  ["② DTLS-SRTP"]="src/crypto/dtls_srtp.cpp"
  ["③ Opus 编解码"]="src/media/opus_codec.cpp"
  ["③ 抖动缓冲"]="src/media/jitter_buffer.cpp"
  ["④ NACK/FEC"]="src/net/loss_protection.cpp"
  ["④ 码率自适应"]="src/net/loss_protection.cpp"
  ["④ 指标统计"]="src/metrics/metrics.cpp"
)
for name in "① 信令服务" "① Trickle ICE" "② STUN/ICE 穿透" "② DTLS-SRTP" \
            "③ Opus 编解码" "③ 抖动缓冲" "④ NACK/FEC" "④ 码率自适应" "④ 指标统计"; do
  ok=1
  for f in ${parts[$name]}; do
    [ -f "$f" ] || { ok=0; echo "  缺文件: $f"; }
  done
  printf "  %-18s %s\n" "$name" "$([ $ok -eq 1 ] && echo '✅ 代码已落地' || echo '❌')"
done

echo ""
echo "=== 每步真实编译运行验证 ==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null 2>&1
cmake --build . -j32 > /dev/null 2>&1
cd $V
echo "  编译: ✅ 成功"
echo ""
printf "  %-22s %s\n" "selftest" "$(cd build && ./selftest 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"
printf "  %-22s %s\n" "ice_test(穿透+Trickle)" "$(cd build && timeout 90 ./ice_test --seconds 1 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"
printf "  %-22s %s\n" "dtls_srtp_test" "$(cd build && timeout 90 ./dtls_srtp_test 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"
printf "  %-22s %s\n" "loss_protection_test" "$(cd build && timeout 60 ./loss_protection_test 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"
printf "  %-22s %s\n" "metrics_test" "$(cd build && timeout 60 ./metrics_test 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"
printf "  %-22s %s\n" "e2e_test(0%)" "$(cd build && timeout 90 ./e2e_test --seconds 2 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"
printf "  %-22s %s\n" "e2e_test(25% loss)" "$(cd build && timeout 90 ./e2e_test --loss 0.25 --seconds 3 2>/dev/null | tail -1 | grep -o '全部通过\|存在失败')"

echo ""
echo "=== 重点记录进仓库文档 ==="
for f in docs/00-architecture.md docs/01-progress-log.md docs/02-pitfalls.md docs/03-verification.md; do
  printf "  %-32s %5d 行\n" "$f" "$(wc -l < "$f")"
done
echo "  踩坑条目数: $(grep -c '^## 坑' docs/02-pitfalls.md)"
echo "  进度日志阶段数: $(grep -c '^## Phase' docs/01-progress-log.md)"

echo ""
echo "=== 信令端到端（服务器 + 双端配对）==="
cd $V/build
./signal_server --port 8899 > /tmp/g_srv.log 2>&1 &
SRV=$!
sleep 1
timeout 15 ./peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 1 > /tmp/g_a.log 2>&1 &
A=$!
sleep 0.6
timeout 15 ./peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 1 > /tmp/g_b.log 2>&1 &
B=$!
wait $A 2>/dev/null; wait $B 2>/dev/null
grep -h "信令面结果" /tmp/g_a.log /tmp/g_b.log | sed 's/^/  /'
grep -c "forwarded" /tmp/g_srv.log | sed 's/^/  服务器转发消息数: /'
kill $SRV 2>/dev/null || true

echo ""
echo "##############################################################"
echo "#  核对完成"
echo "##############################################################"
