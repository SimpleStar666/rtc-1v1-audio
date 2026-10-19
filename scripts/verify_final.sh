#!/bin/bash
# 最终远程验证：全新克隆 -> 编译 -> 全量回归
set -e
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/verify_final
rm -rf $V

echo "=== 1. 全新克隆 ==="
git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
cd $V
echo "  提交: $(git log --oneline -1)"
echo "  总提交数: $(git rev-list --count HEAD)"
echo "  文件数: $(git ls-files | wc -l)"
echo "  含 build/: $(git ls-files | grep -c '^build/' || true)"
echo ""
echo "  源码模块:"
git ls-files 'src/*' | sed 's|/[^/]*$||' | sort -u | sed 's/^/    /'

echo ""
echo "=== 2. 编译 ==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > c.log 2>&1 \
  || { echo "配置失败"; tail -15 c.log; exit 1; }
cmake --build . -j32 > b.log 2>&1 || { echo "编译失败"; grep -E 'error:' b.log | head -10; exit 1; }
echo "  ✅ 编译成功"
echo "  产物: $(ls selftest signal_server peer weaknet_test ice_test dtls_srtp_test loss_protection_test e2e_test 2>/dev/null | tr '\n' ' ')"

echo ""
echo "=== 3. 全量回归 ==="
cd $V
bash scripts/full_regression.sh 2>&1 | tail -14

echo ""
echo "=== 4. 端到端丢包矩阵 ==="
cd $V/build
for L in 0 0.10 0.25; do
  printf "  丢包率 %-6s : " "$L"
  timeout 90 ./e2e_test --loss "$L" --seconds 3 2>/dev/null \
    | grep -E "检测丢包|FEC 恢复|SRTP 加密|结果：" | tr '\n' ' ' | sed 's/  */ /g'
  echo ""
done

echo ""
echo "################ 最终远程验证完成 ################"
