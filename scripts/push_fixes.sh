#!/usr/bin/env bash
# 推送：并发安全修复 + FEC 位宽修复 + 目标码率生效
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"; chmod +x "$WORK/scripts/"*.sh

cd $WORK
git config http.sslVerify false
git add -A
echo "=== 变更 ==="
git status --short | grep -vE 'scripts/(push_|recon_|diag_|test_|prove_|fix_|install_|collect_|patch_|build_call|quick_|final_|add_pitfalls|restore_|verify_as_ts|test_error|test_run|test_playback|test_json|make_|ultimate_|accept_|get_weaknet|characterize|tail_loss|verify_loss|verify_manual|accept_manual|check_baseline|lock_jitter|add_setconfig|sync_back|update_codetour|find_notes|apply_|build_regression)' | head -20

cat > /tmp/m2.txt <<'EOF'
fix: 修真实通话暴露的并发/位宽/空实现三个缺陷（坑 39/40/41）

这三个问题都是"把模块接成真实通话"之后才暴露的，
之前 loopback / e2e 的低负载与默认配置把它们掩盖了。
由代码导读的静态阅读发现，逐条核实后修复。

坑 39 JitterBuffer 无锁 -> 数据竞争
  现象：push() 来自 ICE 接收回调（libnice glib 线程），
        pop() 来自播放线程，两者并发访问 queue_/stats_/水位变量。
  根因：最初只在 loopback 下跑（push/pop 时序温和），
        接上真实 ICE 通道后 push 变成跨线程调用。
        数据竞争不可复现，轻则统计错乱，重则 deque 内部指针损坏而崩溃。
  修复：加 mutable std::mutex，push/pop/ready_to_play/reset/snapshot/
        stats/depth 全部加锁；stats() 从"返回 const 引用"改为"加锁按值返回"
        （返回引用在锁外读取是典型的假线程安全）。
  连带：std::mutex 使类不可赋值，AudioPipeline::init() 的
        `jb_ = JitterBuffer(jcfg)` 改为 `jb_.set_config(jcfg)`。

坑 40 FEC 长度字段 u8 -> 高码率静默截断
  现象：60ms 聚合包在 48kbps 时用户载荷约 360 字节 > 255(u8 上限)，
        长度被静默截断，接收端按错误长度切分 XOR 载荷 -> 恢复出垃圾。
  根因：默认码率 16kbps（120 字节）不触发，所有现有测试都没抓到；
        直到考虑拥塞控制上调到 48kbps 才暴露。
  修复：长度字段 u8 -> u16（编码/解码两侧同步改）。
  验证：新增 scripts/test_fec_big.sh，120/360/900 字节载荷全部恢复正确。

坑 41 set_target_bitrate_bps 是空实现 -> 拥塞控制空转
  现象：实现只有一行 LOGI + 注释"简化：仅记录"。
  后果：GCC 算出目标码率并打印日志，但编码器仍按初始码率编码 ——
        决策与执行断开，自适应成了空转。这类"假实现"比崩溃更危险：
        指标看起来正常，只有对比目标码率与实测码率才能发现对不上。
  修复：直接调 codec_.set_bitrate()（Opus 支持运行时改码率，无需重建编码器）。

文档：
- 02-pitfalls 增至 41 项（新增坑 39/40/41 完整定位过程）
- 05-code-tour 中标注为"边界/未实现"的三处已更新为"已修复"，
  顶部增加同步声明

验证（ts 用户身份）：
  全量回归 8 项全部通过
  真实双进程通话：A/B 各 84 包收发，SRTP 解密 84/84，解码错误 0，退出码 0
  FEC 大载荷：120/360/900 字节全部恢复成功且内容一致
EOF

git commit -q -F /tmp/m2.txt || echo "(无变更)"
echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "推送 rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
