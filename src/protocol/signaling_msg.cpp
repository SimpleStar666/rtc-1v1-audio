#include "protocol/signaling_msg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "signal";

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

// 反转义（解析时用）
std::string json_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[++i]) {
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case '"': o += '"';  break;
                case '\\': o += '\\'; break;
                default: o += s[i];  break;
            }
        } else {
            o += s[i];
        }
    }
    return o;
}
}  // namespace

// ---------------- MiniJson ----------------
void MiniJson::set(const std::string& k, const std::string& v) {
    items_.emplace_back(k, "\"" + json_escape(v) + "\"");
}
void MiniJson::set(const std::string& k, int v) {
    items_.emplace_back(k, std::to_string(v));
}
void MiniJson::set(const std::string& k, bool v) {
    items_.emplace_back(k, v ? "true" : "false");
}
bool MiniJson::is_number(const std::string& v) const {
    if (v.empty()) return false;
    for (char c : v) {
        if (!isdigit(uint8_t(c)) && c != '-' && c != '+' && c != '.') return false;
    }
    return true;
}

std::string MiniJson::dump() const {
    std::string o = "{";
    for (size_t i = 0; i < items_.size(); i++) {
        if (i) o += ",";
        o += "\"" + items_[i].first + "\":" + items_[i].second;
    }
    o += "}";
    return o;
}

bool MiniJson::parse(const std::string& json, std::map<std::string, std::string>* out) {
    out->clear();
    size_t i = json.find('{');
    if (i == std::string::npos) return false;
    i++;

    while (i < json.size()) {
        // 找 key
        size_t k1 = json.find('"', i);
        if (k1 == std::string::npos) break;
        size_t k2 = json.find('"', k1 + 1);
        if (k2 == std::string::npos) break;
        std::string key = json.substr(k1 + 1, k2 - k1 - 1);

        size_t colon = json.find(':', k2);
        if (colon == std::string::npos) break;
        i = colon + 1;

        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;
        if (i >= json.size()) break;

        std::string val;
        if (json[i] == '"') {
            // 字符串值（含转义）
            size_t j = i + 1, start = j;
            std::string raw;
            while (j < json.size()) {
                if (json[j] == '\\') { raw += json[j]; raw += json[j + 1]; j += 2; continue; }
                if (json[j] == '"') break;
                raw += json[j++];
            }
            val = json_unescape(raw);
            i = j + 1;
        } else {
            // 数字/布尔
            size_t j = i;
            while (j < json.size() && json[j] != ',' && json[j] != '}') j++;
            val = json.substr(i, j - i);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\n')) val.pop_back();
            i = j;
        }
        (*out)[key] = val;

        // 跳过逗号
        while (i < json.size() && (json[i] == ',' || json[i] == ' ' || json[i] == '\n')) i++;
        if (i < json.size() && json[i] == '}') break;
    }
    return true;
}

// ---------------- SignalMsg ----------------
const char* signal_type_name(SignalType t) {
    switch (t) {
        case SignalType::kJoin:      return "join";
        case SignalType::kJoined:    return "joined";
        case SignalType::kPeerReady: return "peer_ready";
        case SignalType::kOffer:     return "offer";
        case SignalType::kAnswer:    return "answer";
        case SignalType::kCandidate: return "candidate";
        case SignalType::kBye:       return "bye";
        case SignalType::kPeerLeft:  return "peer_left";
        case SignalType::kPing:      return "ping";
        case SignalType::kStats:     return "stats";
        case SignalType::kError:     return "error";
        default:                     return "unknown";
    }
}

SignalType signal_type_from_name(const std::string& s) {
    if (s == "join")       return SignalType::kJoin;
    if (s == "joined")     return SignalType::kJoined;
    if (s == "peer_ready") return SignalType::kPeerReady;
    if (s == "offer")      return SignalType::kOffer;
    if (s == "answer")     return SignalType::kAnswer;
    if (s == "candidate")  return SignalType::kCandidate;
    if (s == "bye")        return SignalType::kBye;
    if (s == "peer_left")  return SignalType::kPeerLeft;
    if (s == "ping")       return SignalType::kPing;
    if (s == "stats")      return SignalType::kStats;
    if (s == "error")      return SignalType::kError;
    return SignalType::kUnknown;
}

std::string SignalMsg::to_json() const {
    MiniJson j;
    j.set("type", std::string(signal_type_name(type)));
    if (!room.empty())      j.set("room", room);
    if (!peer_id.empty())   j.set("peer_id", peer_id);
    if (!sdp.empty())       j.set("sdp", sdp);
    if (!candidate.empty()) j.set("candidate", candidate);
    if (!mid.empty())       j.set("mid", mid);
    j.set("mline", mline_index);
    if (!reason.empty())    j.set("reason", reason);
    if (type == SignalType::kOffer || type == SignalType::kAnswer) {
        j.set("codec", codec);
        j.set("sample_rate", sample_rate);
        j.set("channels", channels);
        j.set("frame_ms", frame_ms);
    }
    return j.dump();
}

bool SignalMsg::from_json(const std::string& json, SignalMsg* out) {
    std::map<std::string, std::string> kv;
    if (!MiniJson::parse(json, &kv)) {
        LOGE(kTag, "json parse failed: %s", json.c_str());
        return false;
    }
    auto get = [&](const char* k, std::string* v) {
        auto it = kv.find(k);
        if (it != kv.end()) { *v = it->second; return true; }
        return false;
    };
    auto get_int = [&](const char* k, int* v) {
        auto it = kv.find(k);
        if (it != kv.end()) { *v = atoi(it->second.c_str()); return true; }
        return false;
    };

    std::string t;
    if (!get("type", &t)) {
        LOGE(kTag, "missing 'type' field");
        return false;
    }
    out->type = signal_type_from_name(t);
    if (out->type == SignalType::kUnknown) {
        LOGW(kTag, "unknown signal type: %s", t.c_str());
    }

    get("room", &out->room);
    get("peer_id", &out->peer_id);
    get("sdp", &out->sdp);
    get("candidate", &out->candidate);
    get("mid", &out->mid);
    get_int("mline", &out->mline_index);
    get("reason", &out->reason);
    get("codec", &out->codec);
    get_int("sample_rate", &out->sample_rate);
    get_int("channels", &out->channels);
    get_int("frame_ms", &out->frame_ms);
    return true;
}

SignalMsg SignalMsg::make_join(const std::string& room) {
    SignalMsg m; m.type = SignalType::kJoin; m.room = room; return m;
}
SignalMsg SignalMsg::make_offer(const std::string& room, const std::string& sdp) {
    SignalMsg m; m.type = SignalType::kOffer; m.room = room; m.sdp = sdp; return m;
}
SignalMsg SignalMsg::make_answer(const std::string& room, const std::string& sdp) {
    SignalMsg m; m.type = SignalType::kAnswer; m.room = room; m.sdp = sdp; return m;
}
SignalMsg SignalMsg::make_candidate(const std::string& room, const std::string& cand, int mline) {
    SignalMsg m; m.type = SignalType::kCandidate; m.room = room;
    m.candidate = cand; m.mline_index = mline; return m;
}
SignalMsg SignalMsg::make_bye(const std::string& room, const std::string& reason) {
    SignalMsg m; m.type = SignalType::kBye; m.room = room; m.reason = reason; return m;
}

}  // namespace rtc
