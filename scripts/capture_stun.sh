#!/bin/bash
# 抓包确认 STUN 请求是否发出、响应是否回来
IP4=$(getent ahostsv4 stun.l.google.com | awk '{print $1}' | head -1)
echo "IPv4 STUN = $IP4"

cat > /tmp/ip5.c <<EOF
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>
int main(){
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    g_object_set(a,"stun-server","$IP4","stun-server-port",(guint)19302,NULL);
    nice_agent_add_stream(a,1);
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(6000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    gchar* sdp=nice_agent_generate_local_sdp(a);
    int srflx=0; const char* p=sdp;
    if(sdp) while((p=strstr(p,"typ srflx"))){srflx++;p+=9;}
    printf(">>> srflx = %d\n", srflx);
    g_free(sdp); g_object_unref(a);
    return 0;
}
EOF
gcc /tmp/ip5.c -o /tmp/ip5 $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo ""
echo "=== 抓包（后台 tcpdump 6 秒）==="
sudo timeout 8 tcpdump -i any -n "udp and port 19302" -c 8 > /tmp/cap.log 2>&1 &
TCPD=$!
sleep 1

G_MESSAGES_DEBUG=all timeout 10 /tmp/ip5 2>&1 \
  | grep -iE "stun|srflx" | head -12

wait $TCPD 2>/dev/null || true
echo ""
echo "=== tcpdump 结果 ==="
cat /tmp/cap.log 2>/dev/null | head -12 || echo "(无输出——说明 libnice 未发出 UDP 包)"
