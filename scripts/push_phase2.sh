#!/bin/bash
# 推送 Phase 2 成果到 GitHub
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

echo "=== 同步 ==="
for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"

cd $WORK
git config http.sslVerify false
git add -A
echo "变更文件："
git status --short

git commit -q -m "feat(phase2): WebSocket 信令客户端接入，两端配对跑通

新增：
- SignalingClient（libwebsockets 客户端封装）：连接/收发/状态回调
- peer --peer-test 模式：两端经服务器完成 join -> 配对 -> offer/answer -> 候选交换
- 诊断脚本：diag_phase2 / diag_phase2_verbose / gdb_peer / test_phase2

实测（两端 + 服务器三联验证）：
  peer#1 join -> peer#2 join -> paired
  forwarded offer(169B) / answer(170B) / candidate x2(88B)
  两端各收 4 条消息，双向零丢失

本阶段踩坑 4 项（累计 20 项，见 docs/02-pitfalls.md）：
- 坑 16：subprotocol 名两端不一致 -> 握手被拒
- 坑 17：lws per-session user data 放 std::string -> 段错误
         （lws 的 user data 是 malloc 裸内存，不调用 C++ 构造函数）
- 坑 18：lws_write 未预留 LWS_PRE -> 堆越界 \"double free or corruption\"
- 坑 20：发送缓冲用 assign 覆盖 -> 消息丢失（改为 deque 队列）
- 坑 19：消息竞态导致一端误判超时（等待条件改为\"状态达成\"而非\"单条消息\"）

文档：README 重写为准确进度；01-progress-log 补 Phase 2 全流程与实测日志；
      02-pitfalls 新增坑 15~20 并重排速览表" || echo "(无变更)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
