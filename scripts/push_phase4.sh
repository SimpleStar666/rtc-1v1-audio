#!/bin/bash
# 推送 Phase 4（DTLS-SRTP）到 GitHub
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
git status --short | head -20

git commit -q -m "feat(phase4): DTLS-SRTP 媒体加密，21 项验证全部通过

新增：
- DtlsSrtpSession：DTLS 1.2 握手 + RFC 5764 密钥派生 + SRTP 加解密
- tests/dtls_srtp_test：进程内两端完成真实握手并互发加密 RTP
- 诊断脚本：fp_diag（证书可见性）、trace_fail（计数追踪）等

实测（21 项全部通过）：
  协商套件 ECDHE-RSA-AES256-GCM-SHA384
  双向证书指纹互验通过
  RFC 5764 密钥四方一致（client/server key + salt）
  SRTP 加密 35->45 字节（+10 字节 HMAC-SHA1 认证标签），解密内容与明文一致
  篡改密文被拒 / 未加密明文包被拒

本阶段踩坑 3 项（累计 27 项）：
- 坑 25：libsrtp2 API 三处不符预期
         （srtp_t 本身是指针 / 无 keysalt 字段需 key+salt 连续 / most vexing parse）
- 坑 26 ⭐：OpenSSL 的 DTLS server 默认不请求对端证书 -> server 侧读不到对端指纹
         （修复：SSL_VERIFY_PEER + 始终放行的 verify_callback，
           与 WebRTC 不用 CA 证书的策略一致）
- 坑 27 ⭐：测试宏对条件二次求值 -> 带副作用的表达式执行两遍
         （现象：21 项全 PASS 却报'失败 2 项'；修复：先存局部变量）

同时修正 ice_test / selftest 的同类宏缺陷。" || echo "(无变更)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -5
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
