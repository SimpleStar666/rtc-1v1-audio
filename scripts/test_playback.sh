#!/bin/bash
# 正确测试 WSLg 音频是否可播
echo "=== WSLg 音频环境 ==="
echo "  PULSE_SERVER=$PULSE_SERVER"
ls -la /mnt/wslg/PulseServer 2>/dev/null && echo "  ✅ PulseServer socket 存在"
echo ""
echo "=== pactl 检查（PulseAudio 客户端）==="
which pactl paplay 2>/dev/null
echo ""
echo "=== 尝试播放（3 秒超时）==="
if [ -f /tmp/rtc_demo/demo.wav ]; then
  timeout 5 paplay /tmp/rtc_demo/demo.wav 2>&1 && echo "  ✅ paplay 成功（WSL 内可直接播放！）" || echo "  ✗ paplay 失败（退出码 $?）"
fi
echo ""
echo "=== 用 pactl 列出音频设备 ==="
timeout 5 pactl list short sinks 2>&1 | head -5 || echo "  pactl 不可用"
