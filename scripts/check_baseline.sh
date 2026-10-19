#!/bin/bash
# 基线回归检查
cd /home/ts/rtc-1v1-audio/build || { echo "build 目录不存在"; exit 0; }
echo "=== 当前产物 ==="
for f in selftest signal_server peer weaknet_test ice_test dtls_srtp_test; do
  [ -x "$f" ] && echo "  $f"
done
echo ""
echo "=== 快速回归 ==="
echo -n "  selftest:     "; ./selftest 2>/dev/null | tail -1
echo -n "  dtls_srtp:    "; timeout 60 ./dtls_srtp_test 2>/dev/null | tail -1
echo -n "  ice:          "; timeout 40 ./ice_test --seconds 1 2>/dev/null | tail -1
