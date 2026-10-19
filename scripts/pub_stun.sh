#!/bin/bash
# 直接验证 libnice 能否向"真实公网 STUN"取到 srflx
# 若公网也不行，则问题在 libnice 用法；若公网行而本地不行，则是本地 turnserver 配置问题

echo "=== 1. 确认公网 STUN 可达（裸 socket）==="
timeout 10 python3 - <<'PY'
import socket, struct
for host,port in [("stun.l.google.com",19302),("stun1.l.google.com",19302)]:
    try:
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(4)
        s.sendto(b"\x00\x01\x00\x00\x21\x12\xA4\x42"+b"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c",(host,port))
        d,a=s.recvfrom(1024)
        print(f"  {host}:{port} -> {len(d)} 字节响应 (可达)")
    except Exception as e:
        print(f"  {host}:{port} -> 失败 {type(e).__name__}")
PY

echo ""
echo "=== 2. libnice 向公网 STUN 取 srflx ==="
cat > /tmp/pub.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>
int main(){
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    if(!a){printf("agent NULL\n");return 1;}
    g_object_set(a,"stun-server","stun.l.google.com","stun-server-port",(guint)19302,NULL);
    nice_agent_add_stream(a,1);
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(8000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    gchar* sdp=nice_agent_generate_local_sdp(a);
    int srflx=0; const char* p=sdp;
    if(sdp) while((p=strstr(p,"typ srflx"))){srflx++;p+=9;}
    printf("  公网 STUN -> srflx = %d\n", srflx);
    if(sdp){ printf("  SDP:\n"); char* nl=strdup(sdp); char* t=strtok(nl,"\n");
             while(t){ if(strstr(t,"candidate")) printf("    %s\n",t); t=strtok(NULL,"\n"); } free(nl); }
    g_free(sdp); g_object_unref(a);
    return srflx>0?0:1;
}
EOF
gcc /tmp/pub.c -o /tmp/pub $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3
timeout 25 /tmp/pub 2>&1

echo ""
echo "=== 3. 检查 libnice 是否编译了 STUN 支持（HTTP/UPnP 等特性）==="
dpkg -s libnice-dev 2>/dev/null | grep -i version
apt-cache policy libnice10 2>/dev/null | head -3
