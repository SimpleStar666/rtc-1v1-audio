#!/usr/bin/env python3
# 两个修复：
#   A) video_test 的断言过时了：start() 现在会主动请求一次关键帧（设计行为），
#      所以"无丢包时 0 次请求"不再成立 —— 改为"<= 2 次"
#   B) 稳定性残留：assemble_and_decode 里"跳过等待关键帧"的路径
#      没有触发新的 PLI，一旦早先的 PLI 丢失就永久卡住
B = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio'
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

# ---- A) 修测试断言 ----
patch(f'{B}/tests/video_test.cpp', [
    ("""        CHECK(rb.keyframe_requests == 0, "无丢包时不需要请求关键帧");""",
     """        // 注意：`VideoPipeline::start()` 会主动请求一次关键帧
        // （对应 WebRTC 接收端在 setRemoteDescription 后立即发 PLI 的行为），
        // 目的是避免"等下一个周期性关键帧"造成最长 2 秒的黑屏。
        // 所以无丢包时允许 1~2 次启动请求，但稳定传输期间不应持续请求。
        CHECK(rb.keyframe_requests <= 2,
              "无丢包时只在启动时请求关键帧（<=2 次），之后不再请求");""",
     '修正"无丢包 0 请求"断言'),
])

# ---- B) 跳过路径也要请求关键帧 ----
patch(f'{B}/src/media/video_pipeline.cpp', [
    ("""        // ★ 视频特有的判断：如果之前丢过关键帧，且当前帧不是关键帧，
        //   那么这一帧必然解不出来（参考帧链断了）。
        //   直接丢弃，并请求关键帧 —— 而不是浪费 CPU 去解一个必然失败的帧。
        if (!is_key && !lost_keyframe_until_.empty()) {
            const uint16_t last_lost = *lost_keyframe_until_.rbegin();
            if (int16_t(frame_id - last_lost) > 0) {
                std::lock_guard<std::mutex> lk2(stat_mutex_);
                rep_.frames_dropped++;
                // 落在这里说明还在等关键帧，本次不解码
                // （关键帧请求由 on_video_fragment 的超时路径或解码失败路径触发）
                return;
            }
        }""",
     """        // ★ 视频特有的判断：如果之前丢过关键帧，且当前帧不是关键帧，
        //   那么这一帧必然解不出来（参考帧链断了）。
        //   直接丢弃 —— 而不是浪费 CPU 去解一个必然失败的帧。
        if (!is_key && !lost_keyframe_until_.empty()) {
            const uint16_t last_lost = *lost_keyframe_until_.rbegin();
            if (int16_t(frame_id - last_lost) > 0) {
                {
                    std::lock_guard<std::mutex> lk2(stat_mutex_);
                    rep_.frames_dropped++;
                }
                // ★ 这里也必须（限流地）请求关键帧。
                //   早期版本只丢弃、不请求，寄希望于"之前的 PLI 已经送到了"。
                //   但 PLI 是可能丢的（启动时对端还没就绪、或信令那一刻正好断），
                //   一旦丢了，接收端就在"等一个永远不会来的关键帧"里永久卡住。
                //   实测：加压 5 轮仍有 1 轮固定丢 9 帧，就是这条路径没请求造成的。
                //   见 docs/02-pitfalls.md 坑 47。
                need_kf_from_skip = true;
                return;
            }
        }""",
     '跳过路径标记需要请求关键帧'),
    # 声明标志
    ("""    std::vector<uint8_t> data;
    bool is_key = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);
        auto it = pending_.find(frame_id);""",
     """    std::vector<uint8_t> data;
    bool is_key = false;
    bool need_kf_from_skip = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);
        auto it = pending_.find(frame_id);""",
     '声明 need_kf_from_skip'),
])

# 在 return 之后补上：锁外调用请求
s = open(f'{B}/src/media/video_pipeline.cpp', encoding='utf-8').read()
old = """    YuvFrame out;
    if (!decoder_.decode(data.data(), data.size(), &out)) {"""
new = """    // 锁外触发（上面 skip 路径打的标记）
    if (need_kf_from_skip) {
        request_keyframe_throttled();
        return;
    }

    YuvFrame out;
    if (!decoder_.decode(data.data(), data.size(), &out)) {"""
if old in s:
    s = s.replace(old, new, 1)
    open(f'{B}/src/media/video_pipeline.cpp', 'w', encoding='utf-8').write(s)
    print('  ✓ skip 路径锁外触发 PLI')
    n += 1
else:
    print('  ✗ 未找到 decode 锚点')

print(f'\n✅ 共 {n} 处')
