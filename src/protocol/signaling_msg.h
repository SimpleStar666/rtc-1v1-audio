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
    std::string candidate;   // "ip:port"
    std::string mid;         // 媒体标识
    int         mline_index = 0;

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
