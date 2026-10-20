#!/usr/bin/env python3
# 根治启动竞态：
#   A) 发送端开头 1 秒内密集发关键帧（WebRTC 的 "initial keyframe burst" 思路）
#      —— 接收端无论何时就绪，最多等 ~300ms 就能咬合，而不是等 2 秒的周期关键帧
#   B) 修 need_keyframe 的 flag 误消费：
#      `need_keyframe.exchange(false) && video_active.load()` 会先清 flag 再判断，
#      video 未激活时请求被静默吞掉
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

# ---- A) 发送端启动密集关键帧 ----
patch(f'{B}/src/media/video_pipeline.cpp', [
    ("""        EncodedFrame ef;
        if (!codec_.encode(frame, &ef) || ef.data.empty()) {
            continue;
        }""",
     """        // ★ 启动阶段的关键帧密集期（"initial keyframe burst"）。
        //
        //   问题：接收端刚加入时，如果恰好错过一个周期性关键帧，
        //   就要等到下一个（本实现 kf_max_dist = fps*2，即最多 2 秒），
        //   这段时间画面全黑。实测加压 10 轮会有 3 轮出现"固定丢 9 帧"。
        //
        //   为什么不能只靠 PLI：PLI 需要一次往返，而且**首次** PLI 经常会白丢
        //   （对端管线还没起来 / 信令那一刻对端还没进房）。
        //
        //   做法（WebRTC 也是这么做的）：通话开始的头 1 秒内，每 5 帧强制一个
        //   关键帧。这样接收端无论何时就绪，最多等 ~330ms 就能拿到可解码起点。
        //   代价：头 1 秒码率略高（关键帧是 P 帧的 3 倍左右），可接受。
        const int64_t elapsed_ms = now_ms() - send_start_ms_;
        if (elapsed_ms < 1000 and (frames_since_start_ % 5) == 0) {
            codec_.request_keyframe();
        }
        frames_since_start_++;

        EncodedFrame ef;
        if (!codec_.encode(frame, &ef) || ef.data.empty()) {
            continue;
        }""",
     '发送端启动关键帧密集期'),
])

# 成员变量
patch(f'{B}/src/media/video_pipeline.h', [
    ("""    uint16_t next_frame_id_ = 0;""",
     """    uint16_t next_frame_id_ = 0;
    // 自启动以来编码了多少帧（用于"启动关键帧密集期"判断）
    int64_t  frames_since_start_ = 0;""",
     '加 frames_since_start_ 成员'),
])

# ---- B) 修 flag 误消费 ----
patch(f'{B}/src/app/call_session.cpp', [
    ("""        // 控制消息泵（每 100ms 检查一次）：处理对端的关键帧请求
        if (need_keyframe.exchange(false) && video_active.load()) {
            vpipe.on_keyframe_request();
        }""",
     """        // 控制消息泵（每 100ms 检查一次）：处理对端的关键帧请求
        //
        // 注意判断顺序：**先判断 video_active，再清 flag**。
        // 若写成 `need_keyframe.exchange(false) && video_active.load()`，
        // exchange 会先把 flag 清掉，video 未激活时这个请求就被静默吞了 ——
        // 对端会一直等一个永远不会来的关键帧。见 docs/02-pitfalls.md 坑 47。
        if (video_active.load() && need_keyframe.load()) {
            need_keyframe.store(false);
            vpipe.on_keyframe_request();
        }""",
     '修 need_keyframe flag 误消费'),
])

print(f'\n✅ 共 {n} 处')
