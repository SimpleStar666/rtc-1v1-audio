#!/usr/bin/env python3
# 修分片重组 bug：按 frag_index 定位写入，而不是按到达顺序追加
# 这是真实 bug —— UDP 分片会乱序到达，按到达顺序拼接会导致码流字节错乱
# 表现为 "Bitstream not supported by this decoder"
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/media/video_pipeline.cpp'
H = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/media/video_pipeline.h'
n = 0

s = open(P, encoding='utf-8').read()
def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# ---- 1) 修复拼接逻辑 ----
rep("""            pf.got.insert(frag_index);
            pf.frag_len[frag_index] = uint32_t(payload.size() - VideoFragmentHeader::kSize);
            // 简化拼接：按片序追加（真实 RTP 按 offset 写入，这里片序到达即顺序）
            pf.data.insert(pf.data.end(),
                           payload.begin() + VideoFragmentHeader::kSize, payload.end());""",
"""            pf.got.insert(frag_index);
            const uint32_t flen = uint32_t(payload.size() - VideoFragmentHeader::kSize);
            pf.frag_len[frag_index] = flen;

            // ★ 关键修复：必须按 frag_index 写到"该片应该在的位置"，
            //   不能用 `insert(end(), ...)` 按到达顺序追加。
            //
            //   为什么：UDP 不保证分片按序到达。若第 2 片比第 1 片先到，
            //   按到达顺序追加就会把第 2 片的字节放在第 1 片前面，
            //   拼出来的码流字节顺序错乱 → 解码器报
            //   "Bitstream not supported by this decoder"。
            //
            //   这是本项目实测到的真 bug：5 轮通话里有 2 轮 A 端丢 29 帧，
            //   原因正是乱序分片被拼错。见 docs/02-pitfalls.md 坑 45。
            //
            //   正确做法与 RTP 一致：按 offset 定位写入（等价于 RTP 的
            //   fragment offset 语义），片内偏移 = frag_index × max_payload。
            const size_t need = size_t(frag_index) * cfg_.max_fragment_payload + flen;
            if (pf.data.size() < need) pf.data.resize(need, 0);
            memcpy(pf.data.data() + size_t(frag_index) * cfg_.max_fragment_payload,
                   payload.data() + VideoFragmentHeader::kSize, flen);""",
'分片按 index 定位写入')

# ---- 2) 重组完成后按实际总长截断 ----
rep("""    std::vector<uint8_t> data;
    bool is_key = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);
        auto it = pending_.find(frame_id);
        if (it == pending_.end()) return;
        data = std::move(it->second.data);
        is_key = it->second.is_keyframe;
        pending_.erase(it);""",
"""    std::vector<uint8_t> data;
    bool is_key = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);
        auto it = pending_.find(frame_id);
        if (it == pending_.end()) return;
        data = std::move(it->second.data);

        // 按最后一片的实际长度截断：因为按 offset 定位写入时，
        // 缓冲区是按"片索引 × 固定片长"撑开的，末片通常不满，
        // 后面会留一段补零的空白，必须裁掉，否则码流尾部有多余字节。
        const uint16_t last_idx = uint16_t(it->second.frag_count - 1);
        const size_t real_len =
            size_t(last_idx) * cfg_.max_fragment_payload + it->second.frag_len[last_idx];
        if (data.size() > real_len) data.resize(real_len);

        is_key = it->second.is_keyframe;
        pending_.erase(it);""",
'重组后按实际长度截断')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 共 {n} 处修复')
