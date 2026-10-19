#!/usr/bin/env bash
# 验收手册：从全新克隆开始，严格按 docs/04-how-to-run.md 的步骤跑
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/manual_check
rm -rf $V

echo "##############################################################"
echo "#  按手册验收：全新克隆 → 照着手册跑"
echo "##############################################################"

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
cd $V
echo ""
echo "提交: $(git log --oneline -1)"

echo ""
echo "########## 手册 §2 环境准备 ##########"
echo "--- 手册说: ./scripts/run.sh env ---"
bash scripts/run.sh env

echo ""
echo "########## 手册 §1 30 秒上手：命令 ① ##########"
echo "--- 手册说: ./scripts/run.sh test ---"
bash scripts/run.sh test 2>&1 | tail -14

echo ""
echo "########## 手册 §1：命令 ② 本地自环（听声音）##########"
echo "--- 手册说: ./scripts/run.sh loopback ---"
bash scripts/run.sh loopback 2>&1 | tail -12

echo ""
echo "########## 手册 §1：命令 ③ 双端通话 ##########"
echo "--- 手册说: ./scripts/run.sh call ---"
bash scripts/run.sh call 2>&1 | grep -E "信令面结果|forwarded|✓|!|✗" | head -12
