#!/bin/bash
# 侦察：PCM 落盘格式 + 可否播放/转换
cd /home/ts/rtc-1v1-audio

echo "=== 跑一次 loopback 产生 PCM ==="
timeout 30 ./build/peer --loopback --seconds 2 > /dev/null 2>&1
ls -la /tmp/peer_out.pcm 2>/dev/null

echo ""
echo "=== PCM 文件信息 ==="
file /tmp/peer_out.pcm 2>/dev/null
echo "  字节数: $(stat -c%s /tmp/peer_out.pcm 2>/dev/null)"
echo "  按 16kHz/单声道/16bit 换算: $(echo "scale=2; $(stat -c%s /tmp/peer_out.pcm 2>/dev/null)/32000" | bc) 秒"

echo ""
echo "=== 可用音频工具 ==="
for t in ffmpeg ffplay sox aplay paplay; do
  if which $t >/dev/null 2>&1; then echo "  ✅ $t: $(which $t)"; else echo "  ✗ $t 未安装"; fi
done

echo ""
echo "=== Windows 侧可用的播放工具（从 WSL 看）==="
for t in /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
         /mnt/c/Program\ Files/VideoLAN/VLC/vlc.exe; do
  [ -e "$t" ] && echo "  ✅ $t" || echo "  ✗ $t"
done

echo ""
echo "=== 生成测试音频文件的方式（源码里的 PCM 格式）==="
grep -n "audio-file\|read_pcm\|load_pcm" src/app/peer.cpp | head -5
grep -n "kSampleRate\|int16" src/common/util.h | head -5
