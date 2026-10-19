#!/bin/bash
# 最终验收：全新克隆 -> 编译 -> 全部测试（含 Trickle ICE / 指标统计）
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/accept
rm -rf $V

echo "############################################################"
echo "#  最终验收：从 GitHub 全新克隆，独立编译并运行全部测试"
echo "############################################################"
echo ""
echo "=== 1. 克隆 ==="
git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
cd $V
echo "  最新提交: $(git log --oneline -1)"
echo "  提交总数: $(git rev-list --count HEAD)"
echo "  文件总数: $(git ls-files | wc -l)"
echo "  含 build/ 目录: $(git ls-files | grep -c '^build/') 个（应为 0）"
echo "  源码模块数: $(git ls-files 'src/*' | cut -d/ -f2 | sort -u | wc -l)"
echo "  测试文件数: $(git ls-files 'tests/*' | wc -l)"
echo "  文档数: $(git ls-files 'docs/*' | wc -l)"
echo ""
echo "  模块列表:"
git ls-files 'src/*' | cut -d/ -f2 | sort -u | sed 's/^/    /'

echo ""
echo "=== 2. 编译（从零）==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > c.log 2>&1 \
  || { echo "  ❌ cmake 配置失败"; tail -20 c.log; exit 1; }
cmake --build . -j32 > b.log 2>&1 || { echo "  ❌ 编译失败"; grep -E 'error:' b.log | head -15; exit 1; }
echo "  ✅ 编译成功（无 error）"
echo "  产物:"
for f in selftest weaknet_test peer signal_server ice_test dtls_srtp_test loss_protection_test metrics_test e2e_test; do
  [ -x "$f" ] && echo "    $f"
done

echo ""
echo "=== 3. 全部测试 ==="
cd $V
bash scripts/full_regression.sh 2>&1 | tail -13
echo "  metrics_test          $(cd build && timeout 60 ./metrics_test 2>/dev/null | tail -1 | sed 's/^#* *//')"
echo "  ice_test(Trickle)     $(cd build && timeout 90 ./ice_test --seconds 1 2>/dev/null | tail -1 | sed 's/^#* *//')"

echo ""
echo "=== 4. 端到端丢包矩阵 ==="
cd $V/build
for L in 0 0.10 0.25; do
  printf "  loss=%-5s : " "$L"
  timeout 90 ./e2e_test --loss "$L" --seconds 3 2>/dev/null \
    | grep -E "检测丢包|FEC 恢复|SRTP 加密|结果：" | tr '\n' ' ' | sed 's/  */ /g'
  echo ""
done

echo ""
echo "=== 5. 端到端指标报告（无丢包）==="
timeout 90 ./e2e_test --seconds 2 2>/dev/null | grep -A22 "通话指标"

echo ""
echo "############################################################"
echo "#  验收完成"
echo "############################################################"
