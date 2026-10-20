#!/usr/bin/env python3
# 给信令加 kKeyframeRequest 消息类型（视频 PLI 走信令通道）
H = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/protocol/signaling_msg.h'
C = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/protocol/signaling_msg.cpp'

h = open(H, encoding='utf-8').read()
c = open(C, encoding='utf-8').read()

# ---- 头文件：加枚举 ----
if 'kKeyframeRequest' not in h:
    h = h.replace("""    kStats,    // 统计上报""",
"""    kStats,    // 统计上报
    // 视频关键帧请求 —— 对应 WebRTC 的 RTCP PLI / FIR
    //
    // 为什么走信令而不是媒体通道：
    //   媒体通道是 UDP，PLI 本身也可能丢；而且"请求关键帧"是低频控制消息，
    //   走可靠的信令（WebSocket/TCP）更合适。
    //   真实 WebRTC 走 RTCP（与 RTP 复用同一条通道但有独立的可靠性机制），
    //   本项目为简化把它放在信令层 —— 语义等价，路径不同。
    kKeyframeRequest,""", 1)
    open(H, 'w', encoding='utf-8').write(h)
    print('  ✓ 头文件加 kKeyframeRequest')
else:
    print('  - kKeyframeRequest 已存在')

# ---- cpp：名字映射（双向）----
if 'keyframe_request' not in c:
    c = c.replace("""        case SignalType::kStats:     return "stats";""",
"""        case SignalType::kStats:     return "stats";
        case SignalType::kKeyframeRequest: return "keyframe_request";""", 1)
    c = c.replace("""    if (s == "stats")      return SignalType::kStats;""",
"""    if (s == "stats")      return SignalType::kStats;
    if (s == "keyframe_request") return SignalType::kKeyframeRequest;""", 1)
    open(C, 'w', encoding='utf-8').write(c)
    print('  ✓ 名字映射')
else:
    print('  - 名字映射已存在')

# 校验
h2 = open(H, encoding='utf-8').read()
c2 = open(C, encoding='utf-8').read()
print('\n=== 校验 ===')
print('  枚举:', h2.count('kKeyframeRequest'))
print('  名字:', c2.count('keyframe_request'))
