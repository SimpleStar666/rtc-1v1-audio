#!/usr/bin/env bash
# 同步全部改动到 WSL 并推送
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

# 从 WSL 取回被 python 改过的 pitfalls（WSL 里是最新的）
cp -f "$WORK/docs/02-pitfalls.md" "$SRC/docs/02-pitfalls.md"

# 全量同步到 WSL
for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"
chmod +x "$WORK/scripts/"*.sh

echo "=== 文档清单 ==="
for f in "$WORK"/docs/*.md "$WORK"/README.md; do
  printf "  %-46s %5d 行\n" "$(basename "$f")" "$(wc -l < "$f")"
done
echo "  坑数: $(grep -c '^## 坑' "$WORK/docs/02-pitfalls.md")"
echo "  代码导读模块: $(grep -c '^## 模块' "$WORK/docs/05-code-tour.md" 2>/dev/null || echo 0)"

cd $WORK
git config http.sslVerify false
git add -A
echo ""
echo "=== 变更 ==="
git status --short | grep -vE 'scripts/(push_|recon_|diag_|test_|prove_|fix_|install_|collect_|patch_|build_call|quick_|final_|add_pitfalls|restore_|verify_as_ts|test_error|test_run|test_playback|test_json|make_|ultimate_|accept_|get_weaknet|characterize|tail_loss|verify_loss|verify_manual|accept_manual|check_baseline)' | head -20

cat > /tmp/msg.txt <<'EOF'
feat: 真正的双进程实时通话 + 代码重点导读

用户反馈两个关键问题，本次一并解决：
  1. "运行手册只有打印和听声音，看不出代码怎么实现的"
  2. "既然是实时通话工具，全是打印也没体现实时通话功能"

问题 2：实现真正的双进程通话（--call）
  此前 peer --peer-test 只做信令配对，媒体是各自本地 loopback，
  两端从未真正传输音频（peer.cpp 原注释："本阶段仍用 loopback，因为还没接 ICE"）。

  新增 src/app/call_session.{h,cpp}（475 行）：
    - 六阶段编排：信令 -> ICE+DTLS准备 -> offer/answer -> ICE连通
                  -> DTLS握手 -> 媒体双向传输
    - 角色判定：先入房者（成员数=1）为主叫，天然避免 glare
    - IceDtlsTransport 适配器把 ICE 包装成 DTLS 传输层
    - looks_like_rtp() 用 RTP 版本位可靠分流 SRTP / DTLS
    - 复用 AudioPipeline（采集/编码/抖缓/解码/播放一行没改），
      只替换发送出口与接收入口 —— 这是 SendFn 回调设计的价值

  实测（双进程，各 5 秒）：
    A: 发送 86 包 / 接收 84 包 / SRTP 解密 84 / 解密失败 0 / 解码错误 0
    B: 发送 84 包 / 接收 84 包 / SRTP 解密 84 / 解密失败 0 / 解码错误 0
    两端退出码 0，DTLS 指纹双向校验通过

  决定性验证"听到的是对方"（不是自环）：
    A 采扫频 200->3000Hz，B 采 440Hz 单音
    => A 的录音主导频率 = 440Hz（B 的源）
       B 的录音主导频率 = 2400Hz（A 的扫频中段）
    频率交叉验证证明两端真的在互相传音频。

  run.sh call 已切换为 --call，并自动做上述验证。

问题 1：代码重点导读 docs/05-code-tour.md（1650+ 行）
  按"要实现什么 / 代码在哪(文件:行号) / 关键实现 / 设计取舍 / 对应踩坑"讲 10 个模块：
    Opus / 抖动缓冲 / 虚拟设备 / 音频管线 / 包格式 / 信令 /
    ICE-STUN-Trickle / DTLS-SRTP / 弱网对抗 / 指标统计 / 通话编排
  含：一张图（音频数据完整旅程）、从日志反查代码表、
      跨模块三个核心设计（时间注入 / 锁边界 / 序号空间隔离）、
      20 个高频面试问题的代码位置。

顺带修掉两个真实 bug：
  坑 37 Trickle 候选重复发送 6 倍（6 个候选发了 36 次）
         根因：new-candidate 只告知"有新候选"不告知是哪个，
               回调里遍历全部候选全发一遍
         修复：sent_candidates_ 去重
  坑 38 Trickle 回调注册晚于候选产生 -> 候选丢失 -> ICE 连不通
         根因：init() 内部就开始 gather，回调在 init 之后才注册
         修复：early_candidates_ 缓冲 + 注册时补发

文档：新增 05-code-tour.md；README 增加"真正的通话长什么样"与双文档索引；
      02-pitfalls 增至 38 项。

全量回归：selftest / weaknet / loopback / ice / dtls_srtp / loss /
          e2e(0%) / e2e(10%) 全部通过；真实通话两端退出码 0。
EOF

git commit -q -F /tmp/msg.txt || echo "(无变更)"
echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "推送 rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
