#!/usr/bin/env bash
# 快速编译验证（ts 用户）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
cp -r "$SRC/src" /home/ts/rtc-1v1-audio/
chown -R ts:ts /home/ts/rtc-1v1-audio
cd /home/ts/rtc-1v1-audio/build
sudo -u ts bash -c 'export PATH="$HOME/.local/bin:$PATH"; cd /home/ts/rtc-1v1-audio/build && cmake --build . -j32 2>&1 | grep -E "error|Built target" | tail -12'
