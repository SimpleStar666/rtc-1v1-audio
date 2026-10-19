#!/bin/bash
# 用 IPv4 地址设置 STUN（IPv6 在 WSL 不通，导致重传超时）

echo "=== 取 IPv4 地址 ==="
IP=$(getent ahostsv4 stun.l.google.com | awk '{print $1}' | head -1)
echo "  stun.l.google.com (IPv4) -> $IP"

cat > /tmp/ip4.c <<EOF
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
    printf("\n  >>> 用 IPv4 ${IP} -> srflx = %d\n\n", srflx);
    if(sdp){
        char* nl=strdup(sdp); char* t=strtok(nl,"\n");
        while(t){ if(strstr(t,"candidate")) printf("    %s\n",t); t=strtok(NULL,"\n"); }
        free(nl);
    }
    g_free(sdp); g_object_unref(a);
    return srflx>0?0:1;
}
EOF
gcc /tmp/ip4.c -o /tmp/ip4 $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo ""
echo "=== 运行 ==="
timeout 20 /tmp/ip4 2>/dev/null | grep -E "srflx|candidate"
