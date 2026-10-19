#!/bin/bash
# 推送 Phase 3（ICE/STUN 穿透）到 GitHub
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"

cd $WORK
git config http.sslVerify false
git add -A
echo "变更："
git status --short | head -25

git commit -q -m "feat(phase3): libnice ICE/STUN 穿透，实测拿到 srflx 候选并双向传输

新增：
- IceTransport（libnice 封装）：候选收集/交换、连通性检查、媒体收发
- tests/ice_test：两个 agent 建真实 UDP 候选对并双向传输
- 诊断脚本：tcpdump 抓包、libnice 内部日志、多组对照实验

实测：
  host 候选场景：各 6 个候选，候选对选通，双向收发确认内容来自对端
  STUN 场景：stun.l.google.com -> host×6 srflx×3 relay×0（公网映射成功）

本阶段踩坑 4 项（累计 24 项，见 docs/02-pitfalls.md）：
- 坑 21：头文件暴露 glib 类型（guint/gpointer）-> 编译失败
- 坑 22：nice_agent_set_remote_candidates_done 在 libnice 0.1.16 不存在
- 坑 23 ⭐：agent 与 glib 主循环上下文不一致 -> STUN 响应收不到
           （tcpdump 抓包定位：请求发出、响应回来、却持续重传；
            根因是 g_main_context_default() 与 g_main_loop_new(NULL,...) 不是同一上下文）
- 坑 24：libnice 的 stun-server 只接受 IP 不接受域名；且 getaddrinfo 返回 IPv6 导致超时
           （从 libnice.so 的字符串找到属性文档原文确认）" || echo "(无变更)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -4
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
