#!/usr/bin/env bash
# 诊断 CMakeLists 状态
WORK=/home/ts/rtc-1v1-audio
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio

echo "=== C 盘 CMakeLists 里的 vpx/vp8 ==="
grep -n "vpx\|vp8\|VPX" "$SRC/CMakeLists.txt" | head -10

echo ""
echo "=== WSL 侧 CMakeLists 里的 vpx/vp8 ==="
grep -n "vpx\|vp8\|VPX" "$WORK/CMakeLists.txt" | head -10

echo ""
echo "=== 两份是否相同 ==="
diff -q "$SRC/CMakeLists.txt" "$WORK/CMakeLists.txt" && echo "  相同" || echo "  不同"

echo ""
echo "=== WSL 侧 rtc_media 区块 ==="
sed -n '/add_library(rtc_media/,/^)/p' "$WORK/CMakeLists.txt"

echo ""
echo "=== cmake configure 详细输出 ==="
cat > /tmp/cfg.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 2>&1 | tail -20
EOS
chmod +x /tmp/cfg.sh; chown ts:ts /tmp/cfg.sh
sudo -u ts bash /tmp/cfg.sh
