#!/usr/bin/env bash
# 推送：视频稳定性修复（坑 45/46/47）+ 4 个文档补齐
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"; chmod +x "$WORK/scripts/"*.sh

cd $WORK
echo "=== 校验 ==="
echo "  坑数: $(grep -c '^## 坑' docs/02-pitfalls.md)"
for f in docs/*.md README.md; do
  n=$(grep -ciE "VP8|视频|关键帧|PLI" "$f" 2>/dev/null || echo 0)
  printf "  %-34s %5d 行  视频提及 %3d\n" "$f" "$(wc -l < "$f")" "$n"
done

git config http.sslVerify false
git add -A
echo ""
echo "=== 变更（非脚本）==="
git status --short | grep -vE 'scripts/' | head -15

cat > /tmp/mv2.txt <<'EOF'
fix: 多跑几遍才暴露的三个并发/时序 bug（坑 45/46/47）+ 补齐 4 个文档

用户要求"多跑几遍看看有没有问题"。结果 5 轮里有 2~3 轮失败，
查出三个纯音频阶段完全不会暴露的真 bug。

## 坑 45：SRTP 会话被多线程并发使用（间歇性画面全黑）

现象：5 轮 2~3 轮失败，形态固定 —— A 端每帧 "Bitstream not supported"，
B 端完全正常；退出码仍是 0，单次运行看不出问题。

根因：加视频后 dtls.protect() 变成被**两个线程**调用
  （音频发送线程 + 视频发送线程），而 unprotect() 在 glib 网络线程。
  libsrtp 的 srtp_t 会话**不是线程安全的**，并发进入会破坏内部
  重放窗口/序列号状态，输出损坏的密文 —— 接收端解密"成功"但拿到垃圾。
  纯音频时只有 1 个发送线程，所以从没暴露。

修复：收发方向各加一把 mutex（两个 srtp_t 是独立会话，可并发不阻塞）。

## 坑 46：信令服务器不转发 PLI（画面永久卡死不自愈）

现象：A 端"PLI 发出 6 / 收到 0"，B 端根本不知道要发关键帧。

根因：服务器 switch 只处理 join/offer/answer/candidate，
  新增的 kKeyframeRequest 落到 default 被**静默丢弃**
  （编译不报错、运行也不报错）。

修复：服务器转发 kKeyframeRequest。
教训：新增协议消息要同时改 4 处（枚举/名字映射/服务器分发/客户端处理），
  漏一处就静默失效，且症状离根因很远（服务器少个 case → 视频画面卡死）。

## 坑 47：控制消息的处理频率被"统计打印"绑架

现象：修完前两个后仍约 30% 失败，且**固定丢 15 帧**。
  "永远正好 15" 是最强线索：15fps × 1s = 15。

根因两层：
  (a) video_active 直到 vpipe.start() 之后才置 true，
      对端早到的分片（含首个关键帧）被静默丢弃；
  (b) PLI 只在"每秒一次"的统计循环里处理 → 最多等 1 秒才生效。

修复：
  - video_active 在 vpipe.init() 成功后就置位
  - 主循环 1s → 100ms 粒度（控制泵高频、统计低频）
  - start() 时主动请求关键帧（WebRTC 也这么做）
  - 首帧解码前用 100ms 紧急间隔，之后恢复 300ms

## 稳定性收敛过程（这就是"为什么要多跑几遍"）

  加视频后          6/10
  修坑 45           7/10
  修坑 46           7/10
  修坑 47           9/10
  修首帧紧急请求   10/10   ← 最终

如果只跑一遍，这三个问题一个都发现不了。

## 文档补齐（之前只更新了 README/02/05，漏了 4 个）

  docs/00-architecture.md  +96 行：音视频双流架构（SSRC 分流、BUNDLE 简化版、
                                   音视频四差异、SRTP 并发的代价）
                                   并把过时的"后续接入点"表改为"已完成模块清单"
  docs/01-progress-log.md  +185 行：Phase 7（视频）、Phase 8（稳定性两个 bug）
  docs/03-verification.md  +194 行：视频验证（VP8/管线/端到端/画面交叉验证）
                                   + 稳定性收敛曲线表
  docs/04-how-to-run.md    +118 行：怎么"看到画面"（PPM→PNG、三种画面源、
                                   参数说明、花屏排查表）
  02-pitfalls 44 → 47 项
  05-code-tour 顶部加"并发提醒"（坑 39 与 45 是同一类问题）

## 回归

全量测试 8 项全部通过；加压 10 轮（变化分辨率 320x240/640x480、
码率 200~800kbps、三种画面源）全部 PASS，无偶发失败。
EOF

git commit -q -F /tmp/mv2.txt || echo "(无变更)"
echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "推送 rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
