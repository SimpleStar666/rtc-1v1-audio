#!/bin/bash
# 诊断：为什么 DTLS server 侧读不到对端证书指纹
# A=client, B=server。B 报 "B 看到的对端指纹 == A 的本地指纹" 失败

cat > /tmp/fp.c <<'EOF'
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <stdio.h>
int main(){ return 0; }
EOF
echo "已知：A(client) 能读到 B 的指纹；B(server) 读到空。"
echo ""
echo "=== 可能的解释 ==="
echo "DTLS server 在握手完成时，对端证书应当已收到。"
echo "若为空，说明 server 侧的 SSL 对象状态与 client 不同步——"
echo "典型原因是 server 在 SSL_do_handshake 返回 1 之后，"
echo "仍有最后的 Finished 报文未处理，或证书链尚未安装到 session。"
echo ""
echo "=== 检查测试里的时序 ==="
grep -n "th_a\|th_b\|join\|a_ok\|b_ok" /home/ts/rtc-1v1-audio/tests/dtls_srtp_test.cpp | head -20
