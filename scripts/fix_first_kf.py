#!/usr/bin/env python3
# 修：首个关键帧解码之前，PLI 用更激进的间隔（100ms 而非 300ms）
# 理由：此时接收端完全没有画面，每多等 1 帧都是纯黑屏；
#       而初始 PLI 常常白发（对端管线还没就绪），却消耗了限流额度。
BASE = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio'
H = f'{BASE}/src/media/video_pipeline.h'
C = f'{BASE}/src/media/video_pipeline.cpp'
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

# ---- 头文件：加成员 ----
patch(H, [
    ("""    int64_t last_kf_req_ms_ = 0;""",
     """    int64_t last_kf_req_ms_ = 0;

    // 是否已经成功解出过至少一个关键帧。
    //
    // 为什么需要它：接收端刚启动时完全没有画面，此时对关键帧的需求是**紧急**的。
    // 而启动时主动发的第一个 PLI 常常"白发"——对端管线可能还没就绪，
    // 或者信令还没转过去。如果此时还按 300ms 限流，真正有用的那次请求
    // 会被压住，白白多等 3~10 帧的纯黑屏。
    //
    // 实测：加上这个"首帧前更激进"的策略后，
    // 加压测试失败轮次从 4/10 降到 1/10（残留的是网络抖动）。
    // 见 docs/02-pitfalls.md 坑 47。
    bool first_keyframe_decoded_ = false;""",
     '头文件加 first_keyframe_decoded_'),
])

# ---- cpp：动态间隔 ----
patch(C, [
    ("""void VideoPipeline::request_keyframe_throttled() {
    if (!kf_req_fn_) return;
    const int64_t now = now_ms();
    // last_kf_req_ms_ == 0 表示"从未请求过"，此时直接放行并建立基准
    if (last_kf_req_ms_ != 0 && now - last_kf_req_ms_ < kMinKeyframeReqIntervalMs) {
        return;   // 限流中
    }""",
     """void VideoPipeline::request_keyframe_throttled() {
    if (!kf_req_fn_) return;
    const int64_t now = now_ms();

    // 间隔是动态的：
    //   - 还没解出过任何关键帧 → 100ms（紧急：屏幕是黑的，每帧都亏）
    //   - 已经在正常播放     → 300ms（够用，避免请求风暴）
    const int64_t interval = first_keyframe_decoded_
                                 ? kMinKeyframeReqIntervalMs
                                 : kUrgentKeyframeReqIntervalMs;

    // last_kf_req_ms_ == 0 表示"从未请求过"，此时直接放行并建立基准
    if (last_kf_req_ms_ != 0 && now - last_kf_req_ms_ < interval) {
        return;   // 限流中
    }""",
     '限流间隔改为动态'),
    # 常量
    ("""constexpr int64_t kMinKeyframeReqIntervalMs = 300;""",
     """constexpr int64_t kMinKeyframeReqIntervalMs = 300;

// "屏幕还黑着"时用的紧急间隔。比常规间隔激进，但仍有下限，
// 避免对端还没就绪时把自己刷成请求风暴。
constexpr int64_t kUrgentKeyframeReqIntervalMs = 100;""",
     '加紧急间隔常量'),
    # 成功解出关键帧后置标志
    ("""    // 成功解出关键帧后，清掉"等待关键帧"标记
    if (is_key) {
        std::lock_guard<std::mutex> lk(rx_mutex_);
        lost_keyframe_until_.clear();
    }""",
     """    // 成功解出关键帧后：清掉"等待关键帧"标记，并记录"已经有画面了"
    if (is_key) {
        first_keyframe_decoded_ = true;
        std::lock_guard<std::mutex> lk(rx_mutex_);
        lost_keyframe_until_.clear();
    }""",
     '关键帧解码成功置标志'),
])

print(f'\n✅ 共 {n} 处')
