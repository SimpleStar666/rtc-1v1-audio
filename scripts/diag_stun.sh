#!/bin/bash
# 诊断：本地 STUN 服务器下，为什么没有 srflx 候选
# 用 verbose 日志看 libnice 的 STUN 交互

echo "=== 启动本地 STUN（verbose 日志）==="
pkill -f turnserver 2>/dev/null || true
sleep 0.5
cat > /tmp/turn2.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
fingerprint
verbose
log-file=/tmp/turn2.log
CONF
turnserver -c /tmp/turn2.conf > /tmp/turn2_stdout.log 2>&1 &
sleep 2
ss -ulnp 2>/dev/null | grep 3478 && echo "  STUN 在听" || echo "  STUN 未监听"

echo ""
echo "=== 用 STUN 测（agent 侧 verbose）==="
timeout 40 /home/ts/rtc-1v1-audio/build/ice_test --stun 127.0.0.1 --seconds 2 --verbose 2>&1 \
  | grep -iE "stun|srflx|reflexive|gathering|candidate" | head -30

echo ""
echo "=== STUN 服务器侧日志（是否收到 Binding Request）==="
grep -iE "bind|request|allocate|error|session" /tmp/turn2.log 2>/dev/null | head -20 || echo "  (无日志)"
cat /tmp/turn2_stdout.log 2>/dev/null | head -10

echo ""
echo "=== 手工验证 STUN 服务器能正常响应 ==="
timeout 10 python3 - <<'PYEOF'
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(4)
# STUN Binding Request
pkt = b"\x00\x01\x00\x00\x21\x12\xA4\x42" + b"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c"
s.sendto(pkt, ("127.0.0.1", 3478))
try:
    data, addr = s.recvfrom(1024)
    print(f"  收到 {len(data)} 字节，类型=0x{struct.unpack('>H', data[0:2])[0]:04x}")
    # 解析 XOR-MAPPED-ADDRESS
    i = 20
    while i < len(data):
        t, ln = struct.unpack('>HH', data[i:i+4])
        if t == 0x0020:
            port = struct.unpack('>H', data[i+6:i+8])[0] ^ 0x2112
            ip = bytes(b ^ c for b, c in zip(data[i+8:i+12], b"\x21\x12\xA4\x42"))
            print(f"  XOR-MAPPED-ADDRESS: {ip[0]}.{ip[1]}.{ip[2]}.{ip[3]}:{port}")
        i += 4 + ln
except Exception as e:
    print(f"  失败: {e}")
PYEOF

pkill -f turnserver 2>/dev/null || true
