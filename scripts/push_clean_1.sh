#!/bin/bash
# 修正推送：排除 build/、合并远程历史、强制推送干净的项目源码
set -e
TOKEN_FILE="/mnt/c/Users/小陶/.dsh/.mcp-github/token.txt"
TOKEN=$(tr -d '\r\n' < "$TOKEN_FILE")

WORK=/home/ts/rtc-1v1-audio
cd $WORK

# 1) 确保 .gitignore 存在且正确（之前同步时漏掉了）
cat > .gitignore <<'EOF'
# 构建产物
build/
*.o
*.a
*.so

# 日志与临时文件
*.log
cfg.log
c.log
b.log
cmake_configure.log
build.log
cmake_configure.log

# 音频输出
*.pcm
*.wav

# 编辑器
.vscode/
.idea/
compile_commands.json

# 系统
.DS_Store
EOF

# 2) 重建 git 历史：从干净状态重新初始化，避免 build/ 混入
rm -rf .git
git init -q
git config user.email "noreply@github.com"
git config user.name "SimpleStar666"
git config http.sslVerify false

# 3) 只添加应当入库的内容
git add .gitignore CMakeLists.txt README.md src tests scripts docs
echo "=== 将提交的文件 ==="
git status --short
echo ""
echo "文件数: $(git status --short | wc -l)"
echo "（应约为 27 个：源码 + 文档 + 脚本，不含 build/）"
