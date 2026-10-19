#!/bin/bash
# 关键对照：主循环必须迭代"agent 所在的那个上下文"
# 之前的实验里 g_main_loop_new(NULL,...) 迭代的是 thread-default，
# 而 agent 用的是 default context —— 两者不同，STUN 事件无人处理。

pkill -f turnserver 2>/dev/null; sleep 0.5
cat > /tmp/t7.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
fingerprint
log-file=/tmp/t7.log
CONF
turnserver -c /tmp/t7.conf > /dev/null 2>&1 &
sleep 2

cat > /tmp/right.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>

static int try_case(int use_agent_ctx_for_loop, const char* label) {
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    g_object_set(a, "stun-server", "127.0.0.1", "stun-server-port", (guint)3478, NULL);
    nice_agent_add_stream(a, 1);
    nice_agent_attach_recv(a, 1, 1, ctx, NULL, NULL);   // 用同一个 ctx
    nice_agent_gather_candidates(a, 1);

    GMainLoop* loop;
    if (use_agent_ctx_for_loop) {
        loop = g_main_loop_new(ctx, FALSE);              // 迭代 default context
    } else {
        loop = g_main_loop_new(NULL, FALSE);             // 迭代 thread-default
    }
    g_timeout_add(3000, (GSourceFunc)g_main_loop_quit, loop);
    g_main_loop_run(loop);

    gchar* sdp = nice_agent_generate_local_sdp(a);
    int srflx = 0;
    if (sdp) { const char* p = sdp; while ((p = strstr(p, "typ srflx"))) { srflx++; p += 9; } }
    printf("%s -> srflx = %d\n", label, srflx);
    if (sdp) g_free(sdp);
    g_object_unref(a);
    g_main_loop_unref(loop);
    return srflx;
}

int main() {
    int r1 = try_case(1, "loop 迭代 agent 的 ctx（default）");
    int r2 = try_case(0, "loop 迭代 thread-default ctx  ");
    printf("\n结论: %s\n", (r1 > 0) ? "必须让主循环迭代 agent 所在的上下文"
                                    : "两种都不行，另有原因");
    return 0;
}
EOF
gcc /tmp/right.c -o /tmp/right $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo "=== 对照实验 ==="
timeout 30 /tmp/right 2>&1

echo ""
echo "=== STUN 收到的请求数 ==="
grep -icE "bind|request" /tmp/t7.log 2>/dev/null || echo 0

pkill -f turnserver 2>/dev/null || true
