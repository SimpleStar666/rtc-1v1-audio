#!/bin/bash
# 全量回归：所有测试
cd /home/ts/rtc-1v1-audio/build || exit 1
FAIL=0
run() {
  local name="$1"; shift
  printf "  %-22s" "$name"
  out=$(timeout 120 "$@" 2>&1)
  rc=$?
  last=$(echo "$out" | grep -E "结果：" | tail -1)
  if [ -n "$last" ]; then
    echo "$last" | sed 's/^#* *//'
  else
    echo "(无汇总行) rc=$rc"
  fi
  [ $rc -ne 0 ] && FAIL=$((FAIL+1))
}

echo "======== 全量回归 ========"
run "selftest"            ./selftest
run "weaknet_test"        ./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 8
run "loopback"            ./peer --loopback --seconds 2
run "ice_test"            ./ice_test --seconds 2
run "dtls_srtp_test"      ./dtls_srtp_test
run "loss_protection"     ./loss_protection_test
run "e2e_test(0%)"        ./e2e_test --seconds 2
run "e2e_test(10%)"       ./e2e_test --loss 0.10 --seconds 3
echo ""
echo "失败项数: $FAIL"
