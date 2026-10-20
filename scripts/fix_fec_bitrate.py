#!/usr/bin/env python3
# 修两个真实缺陷：
#   1) FEC 长度字段 u8 → u16（60ms 包在 >34kbps 时用户载荷超过 255 字节会被截断）
#   2) AudioPipeline::set_target_bitrate_bps() 从"日志占位"改成真正生效
BASE = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio'
LP = f'{BASE}/src/net/loss_protection.cpp'
AP = f'{BASE}/src/media/audio_pipeline.cpp'

def patch_file(path, patches):
    s = open(path, encoding='utf-8').read()
    for old, new, label in patches:
        if old not in s:
            print(f'  ✗ {label}: 没找到锚点')
            continue
        s = s.replace(old, new, 1)
        print(f'  ✓ {label}')
    open(path, 'w', encoding='utf-8').write(s)

print('=== 1. FEC 长度字段 u8 -> u16 ===')

patch_file(LP, [
    # 编码：注释 + hdr 计算
    (
"""    // 格式：[u16 base_seq][u16 count][u8 各源包长度...][XOR payload]
    const size_t hdr = 2 + 2 + size_t(count_in_group_);""",
"""    // 格式：[u16 base_seq][u16 count][u16 各源包长度...][XOR payload]
    //
    // ★ 长度字段必须是 u16，不能是 u8。
    //   60ms 聚合包在 48kbps 时用户载荷约 360 字节 > 255，
    //   用 u8 会被**静默截断**，接收端按错误长度切分 → FEC 恢复出垃圾数据。
    //   低码率（16kbps 约 120 字节）下不会暴露，所以早期测试没抓到。
    //   见 docs/02-pitfalls.md 坑 40。
    const size_t hdr = 2 + 2 + 2 * size_t(count_in_group_);""",
    '编码：头长度计算 2*count'),
    # 编码：写长度
    (
"""        (*fec_out)[4 + i] = uint8_t(group_payloads_[i].size());""",
"""        put_u16_le(fec_out->data() + 4 + 2 * i, uint16_t(group_payloads_[i].size()));""",
    '编码：写 u16 长度'),
    # 解码：读各包长度求 max
    (
"""        max_len = std::max<size_t>(max_len, fec_pkt[4 + i]);""",
"""        max_len = std::max<size_t>(max_len, get_u16_le(fec_pkt + 4 + 2 * i));""",
    '解码：读 u16 长度求 max_len'),
    # 解码：hdr 计算
    (
"""    const size_t hdr = 4 + size_t(count);""",
"""    const size_t hdr = 4 + 2 * size_t(count);""",
    '解码：头长度计算 2*count'),
    # 解码：按缺失包长度截断
    (
"""        const uint16_t need_len = fec_pkt[4 + (missing[0] - base_seq)];""",
"""        const uint16_t need_len = get_u16_le(fec_pkt + 4 + 2 * size_t(missing[0] - base_seq));""",
    '解码：读 u16 该包长度'),
])

print()
print('=== 2. set_target_bitrate_bps 真正生效 ===')

patch_file(AP, [
    (
"""void AudioPipeline::set_target_bitrate_bps(int bps) {
    LOGI(kTag, "target bitrate -> %d bps", bps);
    // 简化：重建编码器代价高，这里仅记录；真实实现应支持运行时调整
}""",
"""void AudioPipeline::set_target_bitrate_bps(int bps) {
    // ★ 曾经这里只打日志、不真正生效（"简化：仅记录"）。
    //   那等于拥塞控制算出了目标码率却没人执行 —— 自适应成了空转。
    //   Opus 支持运行时改码率（OPUS_SET_BITRATE_REQUEST），直接调即可，
    //   不需要重建编码器。
    //   见 docs/02-pitfalls.md 坑 41。
    if (codec_.set_bitrate(bps)) {
        LOGI(kTag, "target bitrate -> %d bps (applied to encoder)", bps);
    } else {
        LOGW(kTag, "set_target_bitrate_bps(%d) failed", bps);
    }
}""",
    'set_target_bitrate_bps 调用 codec_.set_bitrate'),
])

print()
print('=== 校验 ===')
c = open(LP, encoding='utf-8').read()
print('  剩余 u8 长度引用:', c.count('uint8_t(group_payloads_'))
print('  put_u16_le 长度字段:', c.count('put_u16_le(fec_out->data() + 4 + 2 * i'))
a = open(AP, encoding='utf-8').read()
print('  codec_.set_bitrate 调用:', a.count('codec_.set_bitrate'))
