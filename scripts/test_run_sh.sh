#!/usr/bin/env bash
# 验证 run.sh 的每个子命令真的可用
set -u
cd /home/ts/rtc-1v1-audio
chmod +x scripts/run.sh scripts/make_audio.sh
S=scripts/run.sh

echo "########## help ##########"
bash $S help 2>&1 | head -24

echo ""
echo "########## env ##########"
bash $S env 2>&1 | tail -3

echo ""
echo "########## audio ##########"
bash $S audio 2>&1 | tail -6

echo ""
echo "########## build（已编译过，应快速返回）##########"
bash $S build 2>&1 | tail -6
