#!/bin/bash
# 验证 srflx 候选：本地 coturn STUN（端口 3478）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
cp -r "$SRC/tests" /home/ts/rtc-1v1-audio/
cd /home/ts/rtc-1v1-audio/build
cmake --build . -j32 2>&1 | grep -E "error|Built target ice_test" | head -5

echo ""
echo "=== 启动本地 STUN（3478）==="
pkill -f turnserver 2>/dev/null || true
sleep 0.5
cat > /tmp/turn3.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
fingerprint
log-file=/tmp/turn3.log
CONF
turnserver -c /tmp/turn3.conf > /dev/null 2>&1 &
sleep 2
ss -ulnp 2>/dev/null | grep -q 3478 && echo "  ✅ STUN 监听 3478" || { echo "  ❌ 未监听"; exit 1; }

echo ""
echo "=== ICE 测试（带 STUN）==="
timeout 60 ./ice_test --stun 127.0.0.1 --stun-port 3478 --seconds 3 2>&1 \
  | grep -E "候选类型|选中候选对|收到|结果|PASS|FAIL" | sed 's/^/  /'

pkill -f turnserver 2>/dev/null || true
