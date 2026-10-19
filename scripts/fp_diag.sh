#!/bin/bash
# 深入诊断：DTLS server 侧为何读不到对端证书
cat > /tmp/fpdiag.c <<'EOF'
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// 用真实 UDP socket 做一对 endpoint，检查 server 侧证书可见性
static int srv_fd, cli_fd;
static struct sockaddr_in srv_addr, cli_addr;
static socklen_t cli_len = sizeof(cli_addr);

static SSL_CTX* mk_ctx(int is_client) {
    SSL_CTX* c = SSL_CTX_new(DTLS_method());
    SSL_CTX_set_min_proto_version(c, DTLS1_2_VERSION);
    SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);
    // 自签名证书
    EVP_PKEY* pk = EVP_PKEY_new();
    EVP_PKEY_CTX* pc = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY_keygen_init(pc); EVP_PKEY_CTX_set_rsa_keygen_bits(pc, 2048);
    EVP_PKEY_keygen(pc, &pk);
    X509* x = X509_new();
    X509_set_version(x,2); ASN1_INTEGER_set(X509_get_serialNumber(x),1);
    X509_gmtime_adj(X509_getm_notBefore(x),0); X509_gmtime_adj(X509_getm_notAfter(x),365*24*3600);
    X509_set_pubkey(x,pk);
    X509_NAME* n = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(n,"CN",MBSTRING_ASC,(unsigned char*)(is_client?"CLI":"SRV"),-1,-1,0);
    X509_set_issuer_name(x,n);
    X509_sign(x,pk,EVP_sha256());
    SSL_CTX_use_certificate(c,x); SSL_CTX_use_PrivateKey(c,pk);
    X509_free(x); EVP_PKEY_CTX_free(pc); EVP_PKEY_free(pk);
    return c;
}

int main(){
    srv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    srv_addr.sin_family=AF_INET; srv_addr.sin_port=htons(0);
    srv_addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    bind(srv_fd,(struct sockaddr*)&srv_addr,sizeof(srv_addr));
    getsockname(srv_fd,(struct sockaddr*)&srv_addr,&cli_len);
    printf("server port=%d\n", ntohs(srv_addr.sin_port));

    pid_t pid = fork();
    if (pid == 0) {
        // server
        SSL_CTX* c = mk_ctx(0);
        SSL* s = SSL_new(c);
        BIO* rb = BIO_new_dgram(srv_fd, BIO_NOCLOSE);
        BIO_ctrl(rb, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &cli_addr);
        SSL_set_bio(s, rb, rb);
        SSL_set_accept_state(s);
        int r = SSL_do_handshake(s);
        int loops=0;
        while (r != 1 && loops++ < 2000) {
            usleep(2000);
            r = SSL_do_handshake(s);
        }
        printf("[server] handshake r=%d loops=%d\n", r, loops);
        X509* p = SSL_get_peer_certificate(s);
        printf("[server] SSL_get_peer_certificate = %s\n", p ? "有证书" : "NULL");
        if (p) X509_free(p);
        // 再看 verify 结果
        long vr = SSL_get_verify_result(s);
        printf("[server] verify_result = %ld\n", vr);
        // 打印对端证书链长度
        STACK_OF(X509)* chain = SSL_get_peer_cert_chain(s);
        printf("[server] peer_cert_chain = %s\n", chain ? "非空" : "NULL/空");
        _exit(0);
    }

    usleep(300000);   // 让 server 先起
    // client
    cli_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ca; memset(&ca,0,sizeof(ca));
    ca.sin_family=AF_INET; ca.sin_port=htons(0); ca.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    bind(cli_fd,(struct sockaddr*)&ca,sizeof(ca));
    connect(cli_fd,(struct sockaddr*)&srv_addr,sizeof(srv_addr));
    cli_addr = ca;
    SSL_CTX* c = mk_ctx(1);
    SSL* s = SSL_new(c);
    BIO* rb = BIO_new_dgram(cli_fd, BIO_NOCLOSE);
    BIO_ctrl(rb, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &srv_addr);
    SSL_set_bio(s, rb, rb);
    SSL_set_connect_state(s);
    int r = SSL_do_handshake(s);
    int loops=0;
    while (r != 1 && loops++ < 2000) { usleep(2000); r = SSL_do_handshake(s); }
    printf("[client] handshake r=%d loops=%d\n", r, loops);
    X509* p = SSL_get_peer_certificate(s);
    printf("[client] SSL_get_peer_certificate = %s\n", p ? "有证书" : "NULL");
    if (p) X509_free(p);
    return 0;
}
EOF
gcc /tmp/fpdiag.c -o /tmp/fpdiag $(pkg-config --cflags --libs openssl) 2>&1 | head -5
echo "=== 运行（真实 UDP socket）==="
timeout 25 /tmp/fpdiag 2>&1
