#!/bin/bash
# 开启 libnice 内部调试日志（G_MESSAGES_DEBUG=all）
cat > /tmp/dbg.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>
int main(){
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    g_object_set(a,"stun-server","stun.l.google.com","stun-server-port",(guint)19302,NULL);
    nice_agent_add_stream(a,1);
    nice_agent_attach_recv(a,1,1,ctx,NULL,NULL);
    nice_agent_gather_candidates(a,1);
    GMainLoop* loop=g_main_loop_new(ctx,FALSE);
    g_timeout_add(5000,(GSourceFunc)g_main_loop_quit,loop);
    g_main_loop_run(loop);
    g_object_unref(a);
    return 0;
}
EOF
gcc /tmp/dbg.c -o /tmp/dbg $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo "=== libnice 内部日志 ==="
G_MESSAGES_DEBUG=all timeout 20 /tmp/dbg 2>&1 | grep -iE "stun|gather|candidate|reflex|socket|dns|resolve|error" | head -40
