#pragma once
// 包格式 —— 对应 WebRTC 的 RTP 载荷格式设计
//
// ★ 重要设计约束：本包头与 **RTP 固定头前 12 字节兼容**。
//
// 为什么必须兼容 RTP 头布局：
//   SRTP（libsrtp2）会按标准 RTP 头解析包的字段（版本位、序号、时间戳、SSRC）。
//   如果自定义包的首字节不是 0x80（RTP 版本=2），srtp_protect/unprotect 会
//   拒绝处理，或者解析出错。实测表现：媒体包到达了（153 字节），
//   但"首字节=0x00"导致 RTP 判断失败，直接走了 DTLS 分支。
//   见 docs/02-pitfalls.md 坑 30。
//
// 因此头部布局与 RTP 对齐（前 12 字节），自定义字段放在 RTP 头之后：
//
//   --- RTP 兼容段（12 字节）---
//   [0]      V/P/X/CC   版本位（=0x80）+ 无填充无扩展 + CSRC 计数 0
//   [1]      M/PT       标志位（M）+ 载荷类型（PT，本项目用 96 动态类型）
//   [2..3]   seq        序列号（网络字节序）
//   [4..7]   timestamp  时间戳（网络字节序）
//   [8..11]  ssrc       session_id 映射到的同步源标识（网络字节序）
//   --- 自定义扩展段（8 字节）---
//   [12]     phase      包阶段
//   [13]     flags      标志位
//   [14..15] trans_id   事务 ID（用于打断后过滤在途数据）
//   [16..17] payload_len 载荷长度
//   [18..19] ext_crc    扩展段校验
//
// 总头长 20 字节。这样既满足 SRTP 的 RTP 解析要求，
// 又保留会话语义（trans_id / phase）。
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

// 常量：RTP 头布局
constexpr uint8_t kRtpVersionByte  = 0x80;   // V=2, P=0, X=0, CC=0
constexpr uint8_t kRtpPayloadType  = 96;     // 动态载荷类型（Opus 常用 96~127）

struct PacketHeader {
    uint16_t session_id = 0;      // 映射到 RTP SSRC
    uint16_t seq = 0;             // RTP 序列号
    uint32_t timestamp = 0;       // RTP 时间戳
    Phase    phase = Phase::kBody;
    uint8_t  flags = kFlagNone;
    uint16_t trans_id = 0;
    uint16_t payload_len = 0;
    bool     marker = false;      // RTP M 位（标记重要边界，本项目用于 DING/END）

    static constexpr size_t kSize = 20;   // 12（RTP 兼容）+ 8（自定义扩展）
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

    // 扩展段校验（8 字节自定义头的 CRC）
    static uint16_t ext_crc(const PacketHeader& h);
    static uint16_t header_crc(const PacketHeader& h);   // 兼容别名
};

}  // namespace rtc
