#!/usr/bin/env bash
# 再次推送文档（这次不做 WSL->C 的反向覆盖）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

# 单向：C -> WSL（绝不反向）
for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"
chmod +x "$WORK/scripts/"*.sh

cd $WORK
echo "=== 校验 WSL 侧文档 ==="
echo "  坑数: $(grep -c '^## 坑' docs/02-pitfalls.md)"
echo "  导读声明: $(grep -c '坑 39（JitterBuffer 加锁）' docs/05-code-tour.md)"

git config http.sslVerify false
git add -A
echo ""
echo "=== 待提交（文档部分）==="
git status --short docs/ README.md

cat > /tmp/m3.txt <<'EOF'
docs: 补记坑 39/40/41，代码导读同步"已修复"（承接上一提交的代码修复）

上一提交 e67274b 修了三个缺陷但文档未同步（同步脚本曾被反向覆盖），
本次补齐：

02-pitfalls.md（38 -> 41 项）
  - 坑 39 JitterBuffer 无锁 -> 数据竞争
    含完整分析：为什么 loopback/e2e 掩盖了问题（push 在 loopback 里
    "自己线程调自己"，在真实通话里变成"网络线程调过来"）、
    stats() 返回引用 vs 按值返回的区别、std::mutex 导致类不可赋值的连带修复
  - 坑 40 FEC 长度字段 u8 -> 高码率静默截断
    含位宽对照表（16kbps=120B 不触发 / 48kbps=360B 触发）、
    大载荷验证结果（120/360/900 字节全部恢复正确）
  - 坑 41 set_target_bitrate_bps 空实现 -> 拥塞控制空转
    含"占位实现三种处理方式"的取舍，以及关键检查点：
    "调了这个接口之后，有什么可观测的变化？只有日志就是没实现"

05-code-tour.md
  - 顶部增加同步声明（坑 39/40/41 已修复）
  - FEC 格式说明 u8 -> u16（2 处）
  - FEC "34kbps 以上会截断"改为"已修复，900 字节仍正确"
  - set_target_bitrate_bps "只是日志占位" -> "已真正生效"
  - JitterBuffer "内部不加锁" -> "已内置 std::mutex"（4 处，含表格与 FAQ）

README 踩坑数 38 -> 41。
EOF

git commit -q -F /tmp/m3.txt || echo "(无变更)"
echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "推送 rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
