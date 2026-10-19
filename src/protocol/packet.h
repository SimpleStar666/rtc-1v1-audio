#pragma once
// 应用层包格式 —— 对应 WebRTC 的 RTP 载荷格式设计
//
// 设计目标：把"会话语义"和"传输语义"编码进一个固定包头。
// 对比 RTP 字段（本项目的映射）：
//   RTP SSRC        -> session_id（哪路会话）
//   RTP Sequence    -> seq（丢包检测 / 重排序，16bit 可回绕）
//   RTP Timestamp   -> timestamp（播放时钟，用于抖动计算）
//   RTP PT          -> phase（载荷语义：语音/控制）
//   RTP 扩展头       -> flags + trans_id（会话标识，用于打断过滤）
//
// 包头（16 字节，网络字节序）：
//   [0..1]   session_id   会话标识
//   [2..3]   seq          序列号
//   [4..7]   timestamp    采样时钟（每帧 + samples_per_frame）
//   [8]      phase        阶段（见 Phase 枚举）
//   [9]      flags        标志位（见 Flag 枚举）
//   [10..11] trans_id     事务 ID（用于打断后过滤在途数据）
//   [12..13] payload_len  载荷长度
//   [14..15] header_crc   包头校验（CRC16 简化版）
#include <cstdint>
#include <string>
#include <vector>

namespace rtc {

// 包阶段：不是所有阶段都承载音频，控制包和媒体包复用一个通道
enum class Phase : uint8_t {
    kStart = 0x01,  // 会话开始（携带编解码协商结果）
    kBody  = 0x02,  // 媒体数据（正常音频）
    kEnd   = 0x03,  // 会话结束
    kBreak = 0x04,  // 打断（丢弃在途旧数据）
    kClose = 0x05,  // 关闭
    kPing  = 0x06,  // 心跳
    kPong  = 0x07,  // 心跳应答
    kStats = 0x08,  // 统计上报
};

// 标志位（可按位组合）
enum Flag : uint8_t {
    kFlagNone          = 0x00,
    kFlagUninterruptible = 0x01,  // 不可被打断（如系统提示音）
    kFlagLastFragment  = 0x02,    // 分片传输的最后一个分片
    kFlagFecPacket     = 0x04,    // 这是一个 FEC 冗余包
    kFlagRetransmit    = 0x08,    // 这是重传包（NACK 响应）
};

const char* phase_name(Phase p);

struct PacketHeader {
    uint16_t session_id = 0;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    Phase    phase = Phase::kBody;
    uint8_t  flags = kFlagNone;
    uint16_t trans_id = 0;
    uint16_t payload_len = 0;

    static constexpr size_t kSize = 16;
};

class Packet {
public:
    Packet() = default;

    // 序列化：header + payload 写入 out（返回总字节数，<0 失败）
    static int serialize(const PacketHeader& h, const uint8_t* payload, size_t len,
                         uint8_t* out, size_t out_cap);

    // 反序列化：从 buffer 解析出 header + payload
    static bool parse(const uint8_t* buf, size_t len,
                      PacketHeader* h, std::vector<uint8_t>* payload);

    // 计算包头 CRC（简化：对前 14 字节做累加取反）
    static uint16_t header_crc(const PacketHeader& h);
};

}  // namespace rtc
