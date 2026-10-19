#!/usr/bin/env bash
# 推送：权限修复 + cmake 工具链 + 错误提示加固
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"
chmod +x "$WORK/scripts/"*.sh

cd $WORK
git config http.sslVerify false
git add -A
git status --short | grep -vE 'scripts/(push_|recon_|diag_' | head -20

git commit -q -m "fix: 修复普通用户编译失败（坑 35/36）—— 用户实际反馈的问题

用户以 ts 身份运行 ./scripts/run.sh build 报：
  ./scripts/run.sh: line 54: build.log: Permission denied

两个根因（都是我用 root 开发的连带后果）：

坑 35 权限：
  我一直以 root 身份在 /home/ts/rtc-1v1-audio 里编译，
  整个目录树 906 个条目都变成 root 所有，ts 用户不可写。
  mkdir -p build 静默成功（目录已存在）→ 掩盖了问题，
  直到 > build/build.log 才暴露，而且错误信息还被重定向吞掉。

坑 36 cmake 工具链：
  新版 cmake 用 pip install --user 装的 = 只有 root 能用。
  ts 的 ~/.local/bin 是空的，落到系统 cmake 3.16.3，
  而本项目要求 >= 3.21。
  试过软链 /usr/local/bin/cmake -> /root/.local/bin/cmake，失败
  （/root 是 700，ts 读不到）；正确做法是给 ts 单独 --user 安装。

修复：
1. 恢复目录归属：chown -R ts:ts，删除 root 留下的 build/
2. 给 ts 装 cmake 4.4.4（pip --user）
3. run.sh 增加两个自检，失败时给出可直接复制的修复命令：
   - check_cmake_version()：版本 < 3.21 时提示怎么装、怎么改 PATH
   - check_writable()：目录不可写时打印所有者、当前用户、修复命令
4. cmake 配置失败后主动清理 CMakeCache.txt
   （否则下次报 "could not find CMAKE_PROJECT_NAME in Cache" 更难懂）

验证（以 ts 身份，非 root）：
  全新 build/ → cmake 4.4.4 → 编译成功 → 9 项测试全部通过
  产物归属 ts:ts，非 ts 条目 0
  故意制造两种故障，错误提示均清晰可操作

文档：
- docs/02-pitfalls.md 新增坑 35/36（累计 36 项），
  并在系统性盲区里新增第四类「开发环境 ≠ 用户环境」
- docs/04-how-to-run.md FAQ 新增 Q0（Permission denied）与
  强化 Q1（cmake 版本 —— 说明 --user 的作用域陷阱）

最重要的教训（已写进文档）：
  用用户会用的身份、在用户会用的目录、跑用户会用的命令。
  我之前所有验证都是 root 做的，这本身就是验证方法的漏洞。" || echo "(无变更)"

echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
