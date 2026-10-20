#!/usr/bin/env bash
# 测试真正的双进程通话
cat > /tmp/test_call.sh <<'EOS'
#!/usr/bin/env bash
set -u
cd /home/ts/rtc-1v1-audio/build
export PATH="$HOME/.local/bin:$PATH"

pkill -f signal_server 2>/dev/null; pkill -f "peer --" 2>/dev/null; sleep 0.5

echo "=== 启动信令服务器 (8899) ==="
./signal_server --port 8899 > /tmp/c_srv.log 2>&1 &
SRV=$!
sleep 1
grep -q "server started" /tmp/c_srv.log && echo "  ✓ 服务器就绪" || { echo "  ✗ 失败"; cat /tmp/c_srv.log; exit 1; }

echo ""
echo "=== 启动 A（主叫，先入房）==="
timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
  --seconds 6 --out /tmp/call_A.pcm > /tmp/c_A.log 2>&1 &
PA=$!
sleep 1.5

echo "=== 启动 B（被叫）==="
timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
  --seconds 6 --out /tmp/call_B.pcm > /tmp/c_B.log 2>&1 &
PB=$!

wait $PA 2>/dev/null; RA=$?
wait $PB 2>/dev/null; RB=$?
kill $SRV 2>/dev/null

echo ""
echo "############ A 日志 ############"
cat /tmp/c_A.log
echo ""
echo "############ B 日志 ############"
cat /tmp/c_B.log
echo ""
echo "退出码: A=$RA B=$RB"
EOS
chmod +x /tmp/test_call.sh; chown ts:ts /tmp/test_call.sh
sudo -u ts bash /tmp/test_call.sh
