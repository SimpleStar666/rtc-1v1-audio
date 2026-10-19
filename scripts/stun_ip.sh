#!/bin/bash
# 用"解析后的真实 IP"设置 stun-server（libnice 0.1.16 只接受 IP 而非域名）

echo "=== 解析 STUN 服务器 IP ==="
IP=$(getent hosts stun.l.google.com | awk '{print $1}' | head -1)
echo "  stun.l.google.com -> $IP"

cat > /tmp/ip.c <<EOF
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>
int main(){
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    g_object_set(a,"stun-server","${IP}","stun-server-port",(guint)19302,NULL);
    nice_agent_add_stream(a,1);
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(8000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    gchar* sdp=nice_agent_generate_local_sdp(a);
    int srflx=0; const char* p=sdp;
    if(sdp) while((p=strstr(p,"typ srflx"))){srflx++;p+=9;}
    printf("  用 IP ${IP} -> srflx = %d\n", srflx);
    if(sdp){
        char* nl=strdup(sdp); char* t=strtok(nl,"\n");
        while(t){ if(strstr(t,"candidate")) printf("    %s\n",t); t=strtok(NULL,"\n"); }
        free(nl);
    }
    g_free(sdp); g_object_unref(a);
    return 0;
}
EOF
gcc /tmp/ip.c -o /tmp/ip $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo ""
echo "=== 运行（带 libnice 调试日志）==="
G_MESSAGES_DEBUG=all timeout 20 /tmp/ip 2>&1 | grep -iE "srflx|typ srflx|STUN|candidate.*srflx|反射" | head -15
echo "--- 完整候选列表 ---"
timeout 20 /tmp/ip 2>/dev/null | grep -E "candidate|srflx"
