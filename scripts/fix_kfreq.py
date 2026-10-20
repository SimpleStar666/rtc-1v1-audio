#!/usr/bin/env python3
# 修两个视频关键帧请求的真实缺陷：
#   1) 限流基准 last_kf_req_ms_ 初始为 0，而 now_ms() 是系统运行时长（很大）
#      => now - 0 恒 >= 300 => 限流首次就失效（与坑 28 同类：基准初始化错）
#   2) "缺片导致整帧丢弃"的路径没有请求关键帧
#      => 高丢包时反而不请求，因为帧根本没走到解码失败
#   3) 补一个统一的 request_keyframe_throttled()，两处路径共用
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/media/video_pipeline.cpp'
H = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/media/video_pipeline.h'

s = open(P, encoding='utf-8').read()
n = 0
def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# ---- 1) 新增统一的限流请求函数 ----
rep("""// 重组超时：一帧的分片如果这么久还没凑齐，就放弃（后续帧已到，没必要等）
constexpr int64_t kReassembleTimeoutMs = 300;
}  // namespace""",
"""// 重组超时：一帧的分片如果这么久还没凑齐，就放弃（后续帧已到，没必要等）
constexpr int64_t kReassembleTimeoutMs = 300;
}  // namespace

// 统一的"请求关键帧"入口（带限流）。
//
// 为什么要有这个函数：触发关键帧请求的场景有两个 ——
//   (a) 解码失败（参考帧缺失）
//   (b) 关键帧重组超时/缺片，整帧被丢弃
// 两条路径都要请求，但都必须限流。
//
// ★ 限流基准的初始化是个陷阱：
//   last_kf_req_ms_ 若初始为 0，而 now_ms() 是系统运行时长（可能几十万），
//   则 `now - 0 >= 300` 恒成立 → **限流首次就失效**。
//   这与坑 28（拥塞控制节流基准）是同一类错误：**初始化基准不能是"零值"**。
//   正确做法：第一次请求时用 0 作为哨兵，或把基准初始化到"很久以前"。
void VideoPipeline::request_keyframe_throttled() {
    if (!kf_req_fn_) return;
    const int64_t now = now_ms();
    // last_kf_req_ms_ == 0 表示"从未请求过"，此时直接放行并建立基准
    if (last_kf_req_ms_ != 0 && now - last_kf_req_ms_ < kMinKeyframeReqIntervalMs) {
        return;   // 限流中
    }
    last_kf_req_ms_ = now;
    {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        rep_.keyframe_requests++;
    }
    LOGI(kTag, "requesting keyframe from peer (PLI)");
    kf_req_fn_();
}""",
'新增 request_keyframe_throttled()')

# ---- 2) 关键帧重组超时时请求 ----
rep("""                if (it->second.is_keyframe) {
                    LOGW(kTag, "keyframe %u incomplete (%zu/%u), requesting new one",
                         it->first, it->second.got.size(), it->second.frag_count);
                    lost_keyframe_until_.insert(it->first);
                }""",
"""                if (it->second.is_keyframe) {
                    LOGW(kTag, "keyframe %u incomplete (%zu/%u), requesting new one",
                         it->first, it->second.got.size(), it->second.frag_count);
                    lost_keyframe_until_.insert(it->first);
                    need_kf_req = true;   // 关键帧缺片 → 必须请求
                }""",
'重组超时标记 need_kf_req')

# ---- 3) 在 rx 锁外调用请求（避免持锁调回调）----
rep("""    bool complete = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);""",
"""    bool complete = false;
    bool need_kf_req = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);""",
'声明 need_kf_req')

rep("""        if (pf.got.size() == pf.frag_count) complete = true;
    }

    if (complete) assemble_and_decode(frame_id);""",
"""        if (pf.got.size() == pf.frag_count) complete = true;
    }

    // 关键帧缺片 → 请求新关键帧（在锁外调用，避免持锁回调）
    if (need_kf_req) request_keyframe_throttled();

    if (complete) assemble_and_decode(frame_id);""",
'锁外触发关键帧请求')

# ---- 4) 解码失败路径改用统一函数 ----
rep("""    if (!decoder_.decode(data.data(), data.size(), &out)) {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        rep_.frames_dropped++;
        // 解码失败：极可能参考帧缺失 → 请求关键帧
        if (kf_req_fn_) {
            const int64_t now = now_ms();
            if (now - last_kf_req_ms_ >= kMinKeyframeReqIntervalMs) {
                last_kf_req_ms_ = now;
                rep_.keyframe_requests++;
                kf_req_fn_();
            }
        }
        return;
    }""",
"""    if (!decoder_.decode(data.data(), data.size(), &out)) {
        {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            rep_.frames_dropped++;
        }
        // 解码失败：极可能参考帧缺失 → 请求关键帧（统一入口，自带限流）
        request_keyframe_throttled();
        return;
    }""",
'解码失败改用统一函数')

open(P, 'w', encoding='utf-8').write(s)

# ---- 头文件加声明 ----
h = open(H, encoding='utf-8').read()
if 'request_keyframe_throttled' not in h:
    h = h.replace("""    void send_loop();
    void assemble_and_decode(uint16_t frame_id);""",
"""    void send_loop();
    void assemble_and_decode(uint16_t frame_id);
    // 统一的"请求关键帧"入口（自带限流，见 .cpp 里的说明）
    void request_keyframe_throttled();""", 1)
    open(H, 'w', encoding='utf-8').write(h)
    print('  ✓ 头文件加声明')

print(f'\n✅ 共 {n} 处')
