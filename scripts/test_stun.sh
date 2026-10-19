#!/bin/bash
# 测试 STUN：获得 srflx（公网映射）候选
# 注意：WSL 环境的 TLS 被拦截，但 STUN 是 UDP，可能可以通
BIN=/home/ts/rtc-1v1-audio/build/ice_test

echo "=== STUN 候选测试 ==="
echo "尝试公共 STUN 服务器（UDP 19302）..."
timeout 40 $BIN --stun stun.l.google.com --seconds 3 2>&1 | grep -E "STUN|候选类型|选中候选对|PASS|FAIL|结果"

echo ""
echo "=== 若无 srflx，说明 UDP 出网受限（如实记录）==="
echo "备选：用本机自建 STUN 服务器验证 srflx 逻辑"
