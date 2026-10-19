#!/bin/bash
# 验证属性设置时机：stun-server 必须在 add_stream 之前设置

cat > /tmp/order.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>

static int run(int before, const char* label) {
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    if (before) {
        g_object_set(a,"stun-server","stun.l.google.com","stun-server-port",(guint)19302,NULL);
        nice_agent_add_stream(a,1);
    } else {
        nice_agent_add_stream(a,1);
        g_object_set(a,"stun-server","stun.l.google.com","stun-server-port",(guint)19302,NULL);
    }
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(6000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    gchar* sdp=nice_agent_generate_local_sdp(a);
    int srflx=0; const char* p=sdp;
    if(sdp) while((p=strstr(p,"typ srflx"))){srflx++;p+=9;}
    printf("  %-45s -> srflx = %d\n", label, srflx);
    if(sdp) g_free(sdp); g_object_unref(a); g_main_loop_unref(loop);
    return srflx;
}

int main(){
    int b = run(1, "stun 设置【在】add_stream 之前");
    int af= run(0, "stun 设置【在】add_stream 之后");
    printf("\n  结论: %s\n", b>0 ? "设置时机是关键！必须在 add_stream 之前"
                                : "时机不是原因，继续排查");
    return 0;
}
EOF
gcc /tmp/order.c -o /tmp/order $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3
echo "=== 属性设置时机对照 ==="
timeout 30 /tmp/order 2>&1
