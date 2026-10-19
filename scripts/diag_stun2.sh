#!/bin/bash
# 深入诊断：libnice 是否真的向本地 STUN 发了请求

pkill -f turnserver 2>/dev/null || true
sleep 0.5
cat > /tmp/t4.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
fingerprint
verbose
log-file=/tmp/t4.log
CONF
turnserver -c /tmp/t4.conf > /dev/null 2>&1 &
sleep 2
: > /tmp/t4.log 2>/dev/null || true

echo "=== 跑 ice_test（延长收集超时）==="
# 用较长超时给 STUN 留时间
timeout 60 /home/ts/rtc-1v1-audio/build/ice_test --stun 127.0.0.1 --stun-port 3478 --seconds 2 --verbose 2>&1 \
  | grep -iE "gathering done|srflx|typ host|候选类型|PREF|reflexive|STUN configured" | head -20

echo ""
echo "=== STUN 服务器日志：是否收到 Binding Request ==="
grep -icE "bind|request" /tmp/t4.log 2>/dev/null || echo 0
grep -iE "bind|request|error|session|peer" /tmp/t4.log 2>/dev/null | head -15

echo ""
echo "=== 检查 libnice 的 STUN 连接性 ==="
# 直接看 libnice 尝试连接的目标
ss -uap 2>/dev/null | grep -i turn | head -5

pkill -f turnserver 2>/dev/null || true
