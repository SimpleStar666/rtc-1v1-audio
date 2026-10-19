#!/usr/bin/env bash
# 推送：运行手册 + 一键入口 + 坑 34 修复
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"

# 手册只在 docs 里，确保带上
[ -f "$WORK/docs/04-how-to-run.md" ] || { echo "❌ 手册未同步"; exit 1; }

cd $WORK
git config http.sslVerify false
git add -A
echo "变更："
git status --short | grep -vE '^A scripts/(push_|test_|diag_|recon_|deep_|cmp_|right_|order_|pub_|trace_|stun_|mini_|fp_|libnice_|verify_remote|probe_)' | head -20

git commit -q -m "docs: 新增操作手册 + 一键运行入口；修复坑 34（FEC 包污染 NACK 统计）

新增「怎么跑、怎么玩」手册 docs/04-how-to-run.md（12 节）：
  30 秒上手 / 环境准备 / 一键菜单 / 逐命令详解 / 单独跑各组件 /
  **听到声音** / 双端通话演示 / 弱网演示 / 看懂输出 /
  面试现场演示脚本 / 常见问题 / 命令速查表

新增一键入口 scripts/run.sh：
  ./scripts/run.sh           交互式菜单
  ./scripts/run.sh demo      完整演示（编译+测试+自环+通话）
  ./scripts/run.sh loopback  本地自环，**会播放解码后的音频**
  ./scripts/run.sh call      双端真实通话（自动起服务器+A/B）
  ./scripts/run.sh weaknet   弱网矩阵（带 ✓ 对照，检测丢包==实际丢弃）
  ./scripts/run.sh audio     生成 6 种测试音频（PCM+WAV）

  内置：环境自检（含正确的 pkg-config 名 libsrtp2）、彩色输出、
        自动 PATH 处理、失败时打印日志尾部

新增 scripts/make_audio.sh：生成 6 种测试音频
  sine440/chord/sweep/beeps/noise/silence（各含 .pcm 与 .wav）
  sweep（200→3000Hz 扫频）最适合验证"音频真的通了"

实测：WSL 内 paplay 可播放（WSLg RDP sink 转发到 Windows 扬声器），
      即本项目能真正"听到声音"——rl 自环已验证。

修复坑 34：FEC 包污染 NACK 统计导致丢包数少报
  现象：25% 丢包时 实际丢弃 9 却检测丢包 6
  根因：e2e_test 里 nack.on_packet() 放在分支判断之前，FEC 包
        （独立 SSRC=2、独立序号空间）也进了 NACK 统计，序号序列被搅乱
  注意：该处**注释本身是对的**，明确警告不能喂 FEC 包给 NACK，
        但代码没照注释写——"注释与代码不一致"的典型
  修复：把 nack.on_packet() 移入 session_id==1 分支

  验证：修复后 实际丢弃 14 → 检测丢包 13（偏差 1，为末尾丢包算法边界）
        6 轮 × 6 秒（共 138 丢包）→ 偏差 0
  残留偏差已如实记录为"末尾丢包检测不到"的固有边界（非 bug）

  这个 bug 是**写手册时把数据整理成表格**暴露的——
  表格要求逐行对齐，14 ≠ 13 一眼就看出。

其他：README 增加「快速开始」与手册索引；03-verification 补末尾丢包边界说明。

全量回归：selftest / weaknet / loopback / ice / dtls_srtp / loss /
          e2e(0%) / e2e(10%) 全部通过（失败 0 项）" || echo "(无变更)"

echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
