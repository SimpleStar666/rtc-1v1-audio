#!/bin/bash
# 抓包确认 libnice 是否真的发出 STUN 请求
# 用 strace 看 socket 行为（比 tcpdump 更直接，无需 root 权限问题）

cat > /tmp/trace.c <<'EOF'
#include <nice/agent.h>
int main(){
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    g_object_set(a,"stun-server","stun.l.google.com","stun-server-port",(guint)19302,NULL);
    nice_agent_add_stream(a,1);
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(4000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    g_object_unref(a);
    return 0;
}
EOF
gcc /tmp/trace.c -o /tmp/trace $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo "=== 用 strace 看 DNS 解析与 sendto 目标 ==="
timeout 25 strace -f -e trace=sendto,recvfrom,connect,socket,getaddrinfo -s 200 /tmp/trace 2>&1 \
  | grep -E "sendto|connect|19302|stun" | head -25

echo ""
echo "=== 检查 libnice 是否用了 IPv6 优先（导致 STUN 走 v6 失败）==="
timeout 25 strace -f -e trace=connect -s 120 /tmp/trace 2>&1 | grep -E "AF_INET" | head -15

echo ""
echo "=== 简单验证：libnice 是否支持 DNS 名（vs 仅 IP）==="
cat > /tmp/iponly.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>
int main(){
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    // 直接用 Google STUN 的 IP（跳过 DNS）
    g_object_set(a,"stun-server","74.125.135.127","stun-server-port",(guint)19302,NULL);
    nice_agent_add_stream(a,1);
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(6000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    gchar* sdp=nice_agent_generate_local_sdp(a);
    int srflx=0; const char* p=sdp;
    if(sdp) while((p=strstr(p,"typ srflx"))){srflx++;p+=9;}
    printf("  用 IP 直连 STUN -> srflx = %d\n", srflx);
    g_free(sdp); g_object_unref(a);
    return 0;
}
EOF
gcc /tmp/iponly.c -o /tmp/iponly $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3
timeout 20 /tmp/iponly 2>&1
