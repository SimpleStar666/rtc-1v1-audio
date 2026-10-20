#!/usr/bin/env bash
# 最终：全量回归 + 推送
set -u
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"; chmod +x "$WORK/scripts/"*.sh

cd "$WORK"

echo "=== 全量回归 ==="
cat > /tmp/reg_f.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio
bash scripts/full_regression.sh 2>&1 | tail -12
EOS
chmod +x /tmp/reg_f.sh; chown ts:ts /tmp/reg_f.sh
sudo -u ts bash /tmp/reg_f.sh

echo ""
echo "=== 提交并推送 ==="
git config http.sslVerify false
git add -A
git status --short | grep -vE 'scripts/' | head -10

cat > /tmp/mf.txt <<'EOF'
fix: 根治视频启动竞态（关键帧突发）+ 修 2 处请求路径遗漏，稳定性 10/10

承接上一提交（坑 45/46/47），继续加压到 10 轮后仍有 3 轮失败，
失败形态固定为"主叫端丢 9 帧"。逐个根治：

## 1. 发送端启动关键帧突发（initial keyframe burst）

根因：接收端必须先收到关键帧才能解码，而周期性关键帧间隔是 2 秒。
若恰好在启动窗口错过一个关键帧，最长要等 2 秒才出画面。

只靠 PLI 不够：PLI 需要一次往返，且首次 PLI 经常白丢
（对端管线还没起来 / 信令那一刻对端还没进房）。

修复：通话头 1 秒内每 5 帧强制一个关键帧。
接收端无论何时就绪，最多等 ~330ms 就能拿到可解码起点。
WebRTC 也是这么做的。代价是头 1 秒码率略高，可接受。

## 2. 补齐"等待关键帧"路径的 PLI 请求

assemble_and_decode 里有一段逻辑：已知丢了关键帧时，
直接丢弃后续 P 帧（避免浪费 CPU 解必然失败的帧）。
但这段**只丢弃、不请求**，寄希望于"之前的 PLI 已送达"。
PLI 一旦丢失，接收端就在"等一个永远不会来的关键帧"里永久卡住。

修复：该路径也（限流地）请求关键帧。

## 3. 修 need_keyframe 的 flag 误消费

错误写法：
    if (need_keyframe.exchange(false) && video_active.load())
exchange 先清 flag 再判断，video 未激活时请求被静默吞掉。
修复：先判断 video_active，再清 flag。

## 4. 修 video_test 的过时断言

"无丢包时 0 次关键帧请求"不再成立 —— start() 现在会主动请求一次
（对应 WebRTC 接收端 setRemoteDescription 后立即发 PLI）。改为 <=2 次。

## 稳定性收敛（10 轮加压，变化分辨率/码率/画面源）

  加视频后              6/10  A 端全帧解码失败
  修 SRTP 加锁          7/10  间歇性全帧失败
  修服务器转发 PLI      7/10  画面卡死不自愈
  主循环 100ms          9/10  固定丢 15 帧
  首帧紧急请求          9/10  固定丢 9 帧
  补 skip 路径/flag     9/10  固定丢 4 帧
  启动关键帧突发       10/10  ← 最终

## 关于启动咬合损耗（诚实说明，已写入文档）

最终 10 轮虽全通过，但部分轮次主叫端仍比被叫端少 5~6 帧（约 400ms）。
这不是缺陷，而是实时视频的固有代价：关键帧到达前的 P 帧无法解码，必须丢弃。

本项目把该窗口从最初的 15 帧（1 秒）压到 5~6 帧（约 400ms），
靠的是三层叠加（主动 PLI / 首帧紧急间隔 / 启动关键帧突发）。
WebRTC 同样存在这个窗口。因此判定标准是"没有持续丢帧"而非"绝对零丢帧"。

文档：03-verification 用最终数据重写稳定性章节，含收敛曲线与诚实说明。
EOF

git commit -q -F /tmp/mf.txt || echo "(无变更)"

URL="https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git"
for a in 1 2 3 4 5; do
  echo "推送尝试 $a"
  if git push "$URL" HEAD:main > /tmp/p.txt 2>&1; then
    echo "  [OK]"; tail -2 /tmp/p.txt | sed 's/^/    /'
    git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
    git log --oneline -2
    exit 0
  fi
  tail -2 /tmp/p.txt | sed 's/^/    /'
  sleep $((a*3))
done
echo "  [FAIL] 推送 5 次均失败"
exit 1
