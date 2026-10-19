#!/bin/bash
# 检查 libnice 的 socket 创建情况与 gathering 时序
# 之前的日志显示 gathering 花了 ~15 秒（明显是超时），说明 STUN 阶段在等超时

cat > /tmp/deep.c <<'EOF'
#include <nice/agent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static gboolean on_gathering_done(NiceAgent* a, guint sid, gpointer ud) {
    printf("  [信号] candidate-gathering-done 触发 (stream=%u)\n", sid);
    g_main_loop_quit((GMainLoop*)ud);
    return TRUE;
}

static int check(const char* stun, guint port, gboolean set_ice_udp, const char* label) {
    GMainContext* ctx = g_main_context_default();
    NiceAgent* a = nice_agent_new(ctx, NICE_COMPATIBILITY_RFC5245);
    if (set_ice_udp) g_object_set(a, "ice-udp", TRUE, NULL);
    if (stun) g_object_set(a, "stun-server", stun, "stun-server-port", port, NULL);
    nice_agent_add_stream(a, 1);
    nice_agent_attach_recv(a, 1, 1, ctx, NULL, NULL);

    gchar* s=NULL; guint p=0;
    g_object_get(a, "stun-server", &s, "stun-server-port", &p, NULL);
    printf("  %s: 读回 stun-server=%s:%u\n", label, s?s:"(null)", p);
    if (s) g_free(s);

    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_signal_connect(a, "candidate-gathering-done", G_CALLBACK(on_gathering_done), loop);
    if (!nice_agent_gather_candidates(a, 1)) printf("  %s: gather 调用失败!\n", label);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_timeout_add(12000, (GSourceFunc)g_main_loop_quit, loop);
    g_main_loop_run(loop);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;

    gchar* sdp = nice_agent_generate_local_sdp(a);
    int srflx=0; const char* q=sdp;
    if (sdp) while((q=strstr(q,"typ srflx"))){srflx++;q+=9;}
    printf("  %s: 耗时 %.1fs, srflx=%d\n\n", label, dt, srflx);
    if (sdp) g_free(sdp);
    g_object_unref(a); g_main_loop_unref(loop);
    return srflx;
}

int main(){
    printf("=== 多组合排查 srflx 收集 ===\n");
    check(NULL, 0, TRUE, "无 STUN（基线）    ");
    check("stun.l.google.com", 19302, TRUE,  "公网 STUN + ice-udp");
    check("stun.l.google.com", 19302, FALSE, "公网 STUN 默认     ");
    check("stun.l.google.com", 3478,  TRUE,  "公网 STUN 端口3478 ");
    return 0;
}
EOF
gcc /tmp/deep.c -o /tmp/deep $(pkg-config --cflags --libs nice glib-2.0) 2>&1 | head -3
timeout 80 /tmp/deep 2>&1
