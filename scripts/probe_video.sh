#!/usr/bin/env bash
# 侦察：环境里有哪些视频编解码库可用
echo "=== 1. 已安装的视频相关开发库 ==="
for p in vpx libvpx openh264 x264 x265 libavcodec libavformat libswscale opus; do
  if pkg-config --exists "$p" 2>/dev/null; then
    echo "  ✅ $p : $(pkg-config --modversion "$p")"
  fi
done

echo ""
echo "=== 2. pkg-config 里所有含 vpx/h264/av/265 的包 ==="
pkg-config --list-all 2>/dev/null | grep -iE 'vpx|h264|265|avcodec|swscale|avutil' | awk '{printf "  %-22s %s\n", $1, $2}'

echo ""
echo "=== 3. apt 可安装的候选（不安装，只看） ==="
for p in libvpx-dev libx264-dev libx265-dev libopenh264-dev libavcodec-dev; do
  v=$(apt-cache policy "$p" 2>/dev/null | grep Candidate | awk '{print $2}')
  echo "  $p : ${v:-不可用}"
done

echo ""
echo "=== 4. 现有已安装的运行时库 ==="
ldconfig -p 2>/dev/null | grep -iE 'libvpx|libx264|libx265|libavcodec' | head -8

echo ""
echo "=== 5. 当前项目已有模块 ==="
ls /home/ts/rtc-1v1-audio/src/
echo ""
echo "=== 6. 磁盘空间（编译大库需要） ==="
df -h /home/ts | tail -1
echo ""
echo "=== 7. 内存 ==="
free -h | head -2
