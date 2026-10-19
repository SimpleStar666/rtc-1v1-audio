#!/usr/bin/env bash
cd /home/ts/rtc-1v1-audio
S=scripts/run.sh
echo "########## loopback（应能听到声音）##########"
bash $S loopback 2>&1 | tail -30
echo ""
echo "########## call（双端配对）##########"
bash $S call 2>&1 | tail -35
