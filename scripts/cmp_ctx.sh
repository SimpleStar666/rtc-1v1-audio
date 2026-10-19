#!/bin/bash
# 对照实验：agent 主上下文 vs 主循环上下文是否匹配
# libnice 的 STUN 处理依赖 glib 主上下文被迭代，两者不一致则不会发 STUN 请求

pkill -f turnserver 2>/dev/null; sleep 0.5
cat > /tmp/t6.conf <<'CONF'
listening-port=3478
listening-ip=127.0.0.1
fingerprint
log-file=/tmp/t6.log
CONF
turnserver -c /tmp/t6.conf > /dev/null 2>&1 &
sleep 2

cat > /tmp/cmp.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>

static void run(int use_default_ctx, const char* label) {
    GMainContext* ctx = use_default_ctx ? g_main_context_default() : NULL;
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    if (!a) { printf("%s: agent NULL\n", label); return; }
    g_object_set(a, "stun-server", "127.0.0.1", "stun-server-port", (guint)3478, NULL);
    nice_agent_add_stream(a, 1);
    nice_agent_attach_recv(a, 1, 1, NULL, NULL, NULL);
    nice_agent_gather_candidates(a, 1);

    // 主循环也用默认上下文（与 g_main_loop_new(NULL,...) 一致）
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(3000, (GSourceFunc)g_main_loop_quit, loop);
    g_main_loop_run(loop);

    gchar* sdp = nice_agent_generate_local_sdp(a);
    int srflx = 0;
    if (sdp) { const char* p = sdp; while ((p = strstr(p, "typ srflx"))) { srflx++; p += 9; } }
    printf("%s: srflx 候选数 = %d\n", label, srflx);
    if (sdp) g_free(sdp);
    g_object_unref(a);
}

int main() {
    run(1, "agent 用 g_main_context_default()");
    run(0, "agent 用 NULL（自身上下文）");
    return 0;
}
EOF
gcc /tmp/cmp.c -o /tmp/cmp $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3

echo "=== 对照实验 ==="
timeout 30 /tmp/cmp 2>&1

echo ""
echo "=== STUN 服务器收到的请求数 ==="
grep -icE "bind|request" /tmp/t6.log 2>/dev/null || echo 0
grep -iE "bind|request|session" /tmp/t6.log 2>/dev/null | head -8

pkill -f turnserver 2>/dev/null || true
