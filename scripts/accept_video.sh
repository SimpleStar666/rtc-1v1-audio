#!/usr/bin/env bash
# 最终验收：全新克隆 → 构建 → 11 项测试 → 音视频通话 → 画面验证
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
V=/home/ts/final_video
rm -rf $V

echo "########################################################"
echo "#  最终验收（全新克隆 + ts 用户）"
echo "########################################################"

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
chown -R ts:ts $V; chmod +x $V/scripts/*.sh

cd $V
echo "  提交: $(git log --oneline -1)"
echo "  提交数: $(git rev-list --count HEAD)"
echo "  文件数: $(git ls-files | wc -l)"
echo ""
echo "=== 文档 ==="
for f in docs/*.md README.md; do
  printf "  %-34s %5d 行\n" "$f" "$(wc -l < "$f")"
done
echo "  坑数: $(grep -c '^## 坑' docs/02-pitfalls.md)"
echo "  代码导读模块: $(grep -c '^## 模块' docs/05-code-tour.md)"
echo ""
echo "=== 视频模块 ==="
ls -1 src/media/vp8_codec.* src/media/video_*.* 2>/dev/null | sed 's/^/  /'
echo "  tests: $(ls tests/vp8_test.cpp tests/video_test.cpp 2>/dev/null | wc -l) 个"

cat > /tmp/fv.sh <<EOS
#!/usr/bin/env bash
set -u
export PATH="\$HOME/.local/bin:\$PATH"
cd $V

echo ""
echo "=== 构建 ==="
./scripts/run.sh build 2>&1 | grep -E "libvpx|✓|✗|编译" | head -5

echo ""
echo "=== 11 项测试 ==="
./scripts/run.sh test 2>&1 | grep -E "结果：|✓ 通过|全部通过|失败项" | head -16

echo ""
echo "=== 音视频通话 ==="
./scripts/run.sh call 2>&1 | grep -E "A 端:|B 端:|视频|解码帧落盘|主导频率|采集源|两端通话" | head -14
EOS
chmod +x /tmp/fv.sh; chown ts:ts /tmp/fv.sh
sudo -u ts bash /tmp/fv.sh
