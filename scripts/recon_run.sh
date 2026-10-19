#!/bin/bash
# 侦察：列出所有可用脚本与各程序的实际参数
WORK=/home/ts/rtc-1v1-audio
cd $WORK

echo "=== scripts/ 下所有脚本 ==="
ls -1 scripts/ | sed 's/^/  /'
echo ""
echo "=== 可执行产物 ==="
ls -1 build/ 2>/dev/null | grep -vE '\.(o|log|cmake|txt|json)$' | grep -vE '^CMake' | sed 's/^/  /'
echo ""
echo "=== 各程序的 --help / 无参数输出（看真实参数名）==="
for p in peer signal_server weaknet_test ice_test dtls_srtp_test loss_protection_test metrics_test e2e_test selftest; do
  if [ -x "build/$p" ]; then
    echo "--- $p ---"
    timeout 8 ./build/$p --help 2>&1 | head -14 | sed 's/^/    /'
    echo ""
  fi
done
