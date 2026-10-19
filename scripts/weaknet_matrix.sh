#!/bin/bash
# 弱网测试矩阵：不同丢包/抖动条件下观察抖动缓冲行为
BIN=/home/ts/rtc-1v1-audio/build/weaknet_test
cd /home/ts

echo "################ 弱网测试矩阵 ################"
echo ""
echo "=== A. 理想网络（无丢包无抖动）==="
$BIN --loss 0 --jitter 0 --delay 20 --seconds 10 --jb 60 2>/dev/null | grep -E "发送|丢包帧|抖动缓冲接受|检测丢包|播放帧|PLC|目标延迟|平均抖动|到达延迟|播放连续性"

echo ""
echo "=== B. 轻度丢包（2%）+ 正常抖动 ==="
$BIN --loss 2 --jitter 10 --delay 40 --seconds 10 --jb 60 2>/dev/null | grep -E "发送|丢包帧|抖动缓冲接受|检测丢包|播放帧|PLC|目标延迟|平均抖动|到达延迟|播放连续性"

echo ""
echo "=== C. 中度丢包（8%）+ 明显抖动 ==="
$BIN --loss 8 --jitter 30 --delay 60 --seconds 10 --jb 60 2>/dev/null | grep -E "发送|丢包帧|抖动缓冲接受|检测丢包|播放帧|PLC|目标延迟|平均抖动|到达延迟|播放连续性"

echo ""
echo "=== D. 重度丢包（15%）+ 强抖动 ==="
$BIN --loss 15 --jitter 60 --delay 100 --seconds 10 --jb 60 2>/dev/null | grep -E "发送|丢包帧|抖动缓冲接受|检测丢包|播放帧|PLC|目标延迟|平均抖动|到达延迟|播放连续性"

echo ""
echo "=== E. 大抖动无丢包（考验自适应缓冲）==="
$BIN --loss 0 --jitter 80 --delay 50 --seconds 10 --jb 40 2>/dev/null | grep -E "发送|丢包帧|抖动缓冲接受|检测丢包|播放帧|PLC|目标延迟|平均抖动|到达延迟|播放连续性"
