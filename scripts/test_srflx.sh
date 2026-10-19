#!/bin/bash
# 验证 srflx 候选逻辑：用本机自建 STUN 服务器
# 这能证明"STUN 交互代码正确"，即使外网 UDP 被限制

echo "=== 1. 先确认外网 UDP 是否可用 ==="
timeout 15 python3 - <<'EOF'
import socket
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.connect(("stun.l.google.com", 19302))
    s.send(b"\x00\x01\x00\x00\x21\x12\xA4\x42" + b"\x00"*12)
    data, addr = s.recvfrom(1024)
    print(f"  UDP 出网: 可用（收到 {len(data)} 字节响应）")
except Exception as e:
    print(f"  UDP 出网: 受限 -> {type(e).__name__}: {e}")
EOF

echo ""
echo "=== 2. 安装 coturn 作为本地 STUN 服务器 ==="
dpkg -s coturn >/dev/null 2>&1 || sudo apt-get install -y -qq coturn > /tmp/coturn_install.log 2>&1
if which turnserver >/dev/null 2>&1; then
  echo "  ✅ turnserver 已安装: $(which turnserver)"

  # 启动本地 STUN（只用 STUN，不用 TURN 中继功能，无需鉴权）
  pkill -f turnserver 2>/dev/null || true
  sleep 0.5
  cat > /tmp/turn.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
relay-ip=127.0.0.1
fingerprint
lt-cred-mech
user=test:test
stale-nonce
no-tls
no-dtls
no-cli
log-file=/tmp/turn.log
simple-log
CONF
  turnserver -c /tmp/turn.conf > /tmp/turn_stdout.log 2>&1 &
  sleep 2

  if ss -ulnp 2>/dev/null | grep -q 3478; then
    echo "  ✅ 本地 STUN 监听 127.0.0.1:3478"
    echo ""
    echo "=== 3. 用本地 STUN 测 srflx 候选 ==="
    timeout 40 /home/ts/rtc-1v1-audio/build/ice_test --stun 127.0.0.1 --seconds 3 2>&1 \
      | grep -E "候选类型|选中候选对|收到来自|结果" | sed 's/^/  /'
  else
    echo "  ❌ turnserver 未监听 3478"
    cat /tmp/turn_stdout.log 2>/dev/null | head -10
    cat /tmp/turn.log 2>/dev/null | head -10
  fi
  pkill -f turnserver 2>/dev/null || true
else
  echo "  ❌ coturn 安装失败"; tail -5 /tmp/coturn_install.log
fi
