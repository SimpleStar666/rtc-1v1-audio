#!/usr/bin/env bash
# 验证候选消息的 JSON 往返是否正常
cat > /tmp/json_test.cpp <<'EOF'
#include <cstdio>
#include "protocol/signaling_msg.h"
using namespace rtc;
int main() {
    // 模拟一条真实的 trickle 候选消息
    std::string cand = "a=candidate:1 1 UDP 2013266431 192.168.0.197 46822 typ host";
    SignalMsg m = SignalMsg::make_candidate("demo", cand, 0);
    std::string js = m.to_json();
    printf("=== 序列化后的 JSON ===\n%s\n\n", js.c_str());

    SignalMsg back;
    if (!SignalMsg::from_json(js, &back)) {
        printf("❌ JSON 解析失败！\n");
        return 1;
    }
    printf("解析结果:\n  type=%s\n  room=%s\n  candidate=[%s]\n",
           signal_type_name(back.type), back.room.c_str(), back.candidate.c_str());
    if (back.candidate != cand) {
        printf("❌ 候选内容不一致！原=[%s]\n", cand.c_str());
        return 1;
    }
    printf("✅ 往返一致\n");

    // 再测 offer（含 ICE 凭证 + 指纹，含 '/' '=' 等字符）
    SignalMsg o;
    o.type = SignalType::kOffer;
    o.room = "demo";
    o.ice_ufrag = "rkgP";
    o.ice_pwd   = "q6JwA9lNq6XNqdzIrhRzRl+/=";
    o.fingerprint = "98:F5:89:B2:04:ED:71:BC:30:8C:B4:2E:9B:09:E5:89";
    std::string js2 = o.to_json();
    printf("\n=== offer JSON ===\n%s\n\n", js2.c_str());
    SignalMsg b2;
    if (!SignalMsg::from_json(js2, &b2)) { printf("❌ offer 解析失败\n"); return 1; }
    printf("  ufrag=[%s] pwd=[%s] fp=[%s]\n",
           b2.ice_ufrag.c_str(), b2.ice_pwd.c_str(), b2.fingerprint.c_str());
    if (b2.ice_pwd != o.ice_pwd) { printf("❌ pwd 不一致\n"); return 1; }
    printf("✅ offer 往返一致\n");
    return 0;
}
EOF
cd /home/ts/rtc-1v1-audio
g++ -std=c++17 -I src /tmp/json_test.cpp -o /tmp/json_test \
  build/librtc_protocol.a build/librtc_common.a -lpthread 2>&1 | head -10
/tmp/json_test
