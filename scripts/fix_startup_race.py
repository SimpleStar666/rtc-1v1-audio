#!/usr/bin/env python3
# 修启动竞态：
#   1) VideoPipeline::start() 时主动发一次关键帧请求（WebRTC 也是这么做的）
#   2) call_session 里 vpipe.init() 成功后就置 video_active，
#      避免对端早到的分片被静默丢弃（那正好包含首个关键帧）

BASE = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio'
H = f'{BASE}/src/media/video_pipeline.h'
C = f'{BASE}/src/media/video_pipeline.cpp'
S = f'{BASE}/src/app/call_session.cpp'
n = 0

def patch(path, patches):
    global n
    s = open(path, encoding='utf-8').read()
    for old, new, label in patches:
        if old not in s:
            print(f'  ✗ {label}')
            continue
        s = s.replace(old, new, 1)
        print(f'  ✓ {label}')
        n += 1
    open(path, 'w', encoding='utf-8').write(s)

# ---- 1) 头文件加公开方法 ----
patch(H, [
    ("""    // 收到对端的关键帧请求（对应 RTCP PLI）
    void on_keyframe_request();""",
     """    // 收到对端的关键帧请求（对应 RTCP PLI）
    void on_keyframe_request();

    // 主动请求对端发关键帧（对应接收端主动发 PLI）。
    //
    // 为什么需要"主动"而不是"等失败再请求"：
    //   接收端刚启动时，对端可能已经在发 P 帧了，而 P 帧没有参考帧必然解不出。
    //   如果只在"解码失败"时请求，会有 1 个 RTT 的纯丢帧窗口。
    //   WebRTC 的做法是：接收端一开始就发 PLI（setRemoteDescription 后）。
    //   见 docs/02-pitfalls.md 坑 47。
    void request_keyframe_now();""",
     '头文件加 request_keyframe_now'),
])

# ---- 2) start() 时主动请求 ----
patch(C, [
    ("""void VideoPipeline::start() {
    if (running_) return;
    running_ = true;
    send_start_ms_ = now_ms();
    kbps_window_ms_ = send_start_ms_;
    send_thread_ = std::thread([this] { send_loop(); });
    LOGI(kTag, "started");
}""",
     """void VideoPipeline::start() {
    if (running_) return;
    running_ = true;
    send_start_ms_ = now_ms();
    kbps_window_ms_ = send_start_ms_;
    send_thread_ = std::thread([this] { send_loop(); });

    // ★ 启动时主动请求一次关键帧（不等解码失败）。
    //   否则接收端要等到"下一个周期性关键帧"才能出画面
    //   （本实现 kf_max_dist = fps*2，即最多等 2 秒）。
    //   实测：不加这一行，A 端会固定丢约 15 帧（1 秒）。
    request_keyframe_now();

    LOGI(kTag, "started");
}

void VideoPipeline::request_keyframe_now() {
    // 绕过限流：启动时的一次请求必然要发出去。
    // 用 last_kf_req_ms_ = 0 作为"未请求过"的哨兵（与 throttled 版本一致）。
    if (!kf_req_fn_) return;
    last_kf_req_ms_ = 0;      // 清掉限流基准，确保这次一定发出
    request_keyframe_throttled();
}""",
     'start() 主动请求 + request_keyframe_now 实现'),
])

# ---- 3) call_session：init 成功即置 video_active ----
patch(S, [
    ("""        if (!vpipe.init(vcfg, &vdevice)) {
            fprintf(stderr, "  ! 视频管线初始化失败，降级为纯音频\\n");
            video_ready = false;
        } else {""",
     """        if (!vpipe.init(vcfg, &vdevice)) {
            fprintf(stderr, "  ! 视频管线初始化失败，降级为纯音频\\n");
            video_ready = false;
        } else {
            // ★ 初始化完成就置 active —— 不要等到 start() 之后。
            //   原因：ICE 接收回调靠 video_active 判断是否把分片交给视频管线。
            //   若置得太晚，对端早到的分片（正好包含**首个关键帧**）会被静默丢弃，
            //   表现为主叫端固定丢约 15 帧且不触发 PLI（因为根本没走到解码）。
            //   见 docs/02-pitfalls.md 坑 47。
            video_active = true;""",
     '  init 成功即置 video_active（早到分片不丢）'),
])

print(f'\n✅ 共 {n} 处修复')
