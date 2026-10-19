#!/bin/bash
# 最小化复现：单 agent + STUN，观察 srflx 收集
# 用 tcpdump 抓 UDP 3478 的包，确认请求是否发出

pkill -f turnserver 2>/dev/null; sleep 0.5
cat > /tmp/t5.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
fingerprint
verbose
log-file=/tmp/t5.log
CONF
turnserver -c /tmp/t5.conf > /dev/null 2>&1 &
sleep 2
: > /tmp/t5.log

echo "=== 检查 libnice 版本特性（是否需要设置 ice-udp / upnp）==="
cat > /tmp/mini.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
int main(){
    NiceAgent* a = nice_agent_new(NULL, NICE_COMPATIBILITY_RFC5245);
    if(!a){printf("agent NULL\n");return 1;}
    gboolean ice_udp=FALSE; gchar* stun=NULL; guint port=0;
    g_object_get(a, "ice-udp", &ice_udp, NULL);
    g_object_get(a, "stun-server", &stun, "stun-server-port", &port, NULL);
    printf("默认 ice-udp=%d, stun-server=%s, port=%u\n", ice_udp, stun?stun:"(null)", port);
    nice_agent_add_stream(a, 1);
    nice_agent_attach_recv(a, 1, 1, NULL, NULL, NULL);
    g_object_set(a, "stun-server", "127.0.0.1", "stun-server-port", (guint)3478, NULL);
    // 打印实际设置后的值
    stun=NULL; port=0;
    g_object_get(a, "stun-server", &stun, "stun-server-port", &port, NULL);
    printf("设置后 stun-server=%s, port=%u\n", stun?stun:"(null)", port);
    nice_agent_gather_candidates(a, 1);
    printf("gather 已调用\n");
    // 跑主循环 3 秒
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(3000, (GSourceFunc)g_main_loop_quit, loop);
    g_main_loop_run(loop);
    gchar* sdp = nice_agent_generate_local_sdp(a);
    printf("=== SDP ===\n%s\n", sdp ? sdp : "(null)");
    if(sdp) g_free(sdp);
    g_object_unref(a);
    return 0;
}
EOF
gcc /tmp/mini.c -o /tmp/mini $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -5
echo "--- 运行 ---"
timeout 20 /tmp/mini 2>&1 | grep -vE "^$"

echo ""
echo "=== STUN 服务器是否收到请求 ==="
grep -icE "bind|request|session" /tmp/t5.log || echo 0
head -20 /tmp/t5.log

pkill -f turnserver 2>/dev/null || true
