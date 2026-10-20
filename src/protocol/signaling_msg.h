#pragma once
// 信令消息 —— WebRTC 标准不规定信令协议（JSEP 只管 SDP 协商），
// 所以这里自定义一套 JSON 信令，用于：
//   1. 房间管理（入房/配对/离房）
//   2. 会话描述协商（offer / answer）
//   3. ICE 候选交换（candidate，Trickle ICE）
//
// 最小集就这三类 + 房间事件，正好对应 WebRTC 1v1 建链所需。
#include <map>
#include <string>
#include <vector>

namespace rtc {

enum class SignalType : uint8_t {
    kUnknown = 0,
    kJoin,     // 客户端 -> 服务端：请求加入房间
    kJoined,   // 服务端 -> 客户端：入房成功，告知对端是否就绪
    kPeerReady,// 服务端 -> 客户端：对端已就绪，可以开始协商
    kOffer,    // 会话描述：offer
    kAnswer,   // 会话描述：answer
    kCandidate,// ICE 候选
    kBye,      // 主动挂断
    kPeerLeft, // 服务端 -> 客户端：对端离开
    kPing,     // 应用层心跳
    kStats,    // 统计上报
    // 视频关键帧请求 —— 对应 WebRTC 的 RTCP PLI / FIR
    //
    // 为什么走信令而不是媒体通道：
    //   媒体通道是 UDP，PLI 本身也可能丢；而且"请求关键帧"是低频控制消息，
    //   走可靠的信令（WebSocket/TCP）更合适。
    //   真实 WebRTC 走 RTCP（与 RTP 复用同一条通道但有独立的可靠性机制），
    //   本项目为简化把它放在信令层 —— 语义等价，路径不同。
    kKeyframeRequest,
    kError,    // 错误
};

const char* signal_type_name(SignalType t);
SignalType  signal_type_from_name(const std::string& s);

struct SignalMsg {
    SignalType type = SignalType::kUnknown;

    // 通用字段
    std::string room;
    std::string peer_id;

    // 会话描述（offer/answer）
    std::string sdp;
    // ICE 候选
    std::string candidate;   // "ip:port" 或 a=candidate:... 的 SDP 片段
    std::string mid;         // 媒体标识
    int         mline_index = 0;

    // ---- ICE 凭证（真实通话必需）----
    //
    // 为什么必须用独立字段、而不是塞进 sdp 字符串再解析：
    //   libnice 生成的 ufrag/pwd 是随机串，可能含 '+' '/' '=' 等字符
    //   （实测 pwd 里出现过 '/'）。若用 "ufrag=xxx;pwd=yyy" 这种拼接格式，
    //   '=' 会把 key=value 切错。用独立 JSON 字段最稳。
    //
    // 见 docs/02-pitfalls.md 坑 31（Trickle ICE 必须交换凭证）。
    std::string ice_ufrag;
    std::string ice_pwd;

    // ---- DTLS 证书指纹（防 MITM）----
    // 格式 "AA:BB:CC:..."（SHA-256），对应 SDP 的 a=fingerprint 行。
    std::string fingerprint;

    // 错误信息
    std::string reason;

    // 编解码协商结果（简化版 SDP 语义）
    std::string codec = "opus";
    int         sample_rate = 16000;
    int         channels = 1;
    int         frame_ms = 20;

    // 序列化为 JSON 字符串
    std::string to_json() const;
    // 从 JSON 解析（返回 false 表示格式错误）
    static bool from_json(const std::string& json, SignalMsg* out);

    // 便捷构造
    static SignalMsg make_join(const std::string& room);
    static SignalMsg make_offer(const std::string& room, const std::string& sdp);
    static SignalMsg make_answer(const std::string& room, const std::string& sdp);
    static SignalMsg make_candidate(const std::string& room, const std::string& cand, int mline);
    static SignalMsg make_bye(const std::string& room, const std::string& reason);
};

// ---- 极简 JSON 读写（避免引入外部 JSON 库，本项目只需扁平对象）----
class MiniJson {
public:
    void set(const std::string& k, const std::string& v);
    void set(const std::string& k, int v);
    void set(const std::string& k, bool v);
    std::string dump() const;

    static bool parse(const std::string& json, std::map<std::string, std::string>* out);

private:
    std::vector<std::pair<std::string, std::string>> items_;
    bool is_number(const std::string& v) const;
};

}  // namespace rtc
