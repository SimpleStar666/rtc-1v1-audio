#!/bin/bash
# 诊断 loopback 卡死：dump 各线程的调用栈
cd /home/ts/rtc-1v1-audio/build
./peer --loopback --seconds 3 > /tmp/lo.log 2>&1 &
PEER_PID=$!
echo "peer pid=$PEER_PID，等待 12 秒让它有机会卡住..."
sleep 12

if ! kill -0 $PEER_PID 2>/dev/null; then
  echo "=== 进程已正常退出（没有卡死）==="
  cat /tmp/lo.log
  exit 0
fi

echo "=== 进程仍在运行 —— 卡死确认 ==="
echo "--- 线程列表 ---"
ls /proc/$PEER_PID/task/ 2>/dev/null
echo ""
echo "--- 各线程 wchan（内核等待点）---"
for t in /proc/$PEER_PID/task/*/; do
  tid=$(basename $t)
  name=$(cat $t/comm 2>/dev/null)
  wchan=$(cat $t/wchan 2>/dev/null)
  state=$(awk '{print $3}' $t/stat 2>/dev/null)
  echo "  tid=$tid name=$name state=$state wchan=$wchan"
done
echo ""
echo "--- 用 gdb 获取用户态栈（若可用）---"
if command -v gdb >/dev/null 2>&1; then
  gdb -p $PEER_PID -batch -ex "thread apply all bt" 2>&1 | grep -E "^#|^Thread" | head -40
else
  echo "gdb 不可用，改用 /proc/PID/task/*/stack"
  for t in /proc/$PEER_PID/task/*/; do
    echo "--- $(cat $t/comm) ---"
    head -8 $t/stack 2>/dev/null || echo "  (无权限或不可用)"
  done
fi

echo ""
echo "--- 已输出的日志 ---"
cat /tmp/lo.log
kill -9 $PEER_PID 2>/dev/null
