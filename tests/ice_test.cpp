// ICE 穿透测试：两个 IceTransport 实例在同机建立真实 UDP 候选对
//
// 验证内容：
//   1. 候选收集（host 候选至少要有）
//   2. 双向交换候选 SDP
//   3. 连通性检查选出可用候选对
//   4. 在选中的候选对上双向收发媒体数据（这是真正的"穿透"验证）
//
// 与 WebRTC 的对应：这就是 1v1 建链中 ICE 部分的完整流程。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "ice/ice_transport.h"

using namespace rtc;

static int g_fail = 0;
// 条件只求值一次（避免带副作用的表达式被执行两遍）。见 docs/02-pitfalls.md 坑 27。
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

int main(int argc, char** argv) {
    const char* stun = nullptr;
    int stun_port = 19302;      // 默认公共 STUN 端口
    int seconds = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stun") && i + 1 < argc) stun = argv[++i];
        else if (!strcmp(argv[i], "--stun-port") && i + 1 < argc) stun_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ ICE 穿透测试 ################\n");
    printf("STUN 服务器: %s\n", stun ? stun : "(未配置，仅 host 候选)");
    printf("说明：两个 agent 在同机运行，用 host 候选即可连通；\n");
    printf("      配置 STUN 时会额外获得 srflx 候选（公网映射）。\n\n");

    // ---- 两个 agent ----
    IceTransport::Config ca;
    ca.stream_id = 1;
    ca.controlling = true;      // A 为主控（对应主叫）
    if (stun) { ca.stun_server = stun; ca.stun_port = stun_port; }

    IceTransport::Config cb = ca;
    cb.controlling = false;     // B 为受控（对应被叫）

    IceTransport a, b;

    std::atomic<int> a_recv{0}, b_recv{0};
    std::atomic<int> a_bytes{0}, b_bytes{0};
    std::vector<uint8_t> a_got, b_got;

    a.set_on_recv([&](const uint8_t* d, size_t n) {
        a_recv++; a_bytes += int(n);
        a_got.assign(d, d + n);
    });
    b.set_on_recv([&](const uint8_t* d, size_t n) {
        b_recv++; b_bytes += int(n);
        b_got.assign(d, d + n);
    });

    printf("=== 1. 初始化两个 agent 并收集候选 ===\n");
    CHECK(a.init(ca), "agent A 初始化");
    CHECK(b.init(cb), "agent B 初始化");

    const bool a_gathered = a.wait_gathering_done(8000);
    const bool b_gathered = b.wait_gathering_done(8000);
    CHECK(a_gathered, "A 候选收集完成");
    CHECK(b_gathered, "B 候选收集完成");

    const std::string a_sdp = a.local_sdp();
    const std::string b_sdp = b.local_sdp();
    printf("    A 本地候选 SDP: %zu 字节\n", a_sdp.size());
    printf("    B 本地候选 SDP: %zu 字节\n", b_sdp.size());
    CHECK(!a_sdp.empty(), "A 产生了本地候选");
    CHECK(!b_sdp.empty(), "B 产生了本地候选");

    // 候选类型统计（验证 host / srflx）
    auto count_type = [](const std::string& s, const char* t) {
        size_t n = 0, pos = 0;
        while ((pos = s.find(t, pos)) != std::string::npos) { n++; pos += strlen(t); }
        return n;
    };
    printf("    A 候选类型: host×%zu srflx×%zu relay×%zu\n",
           count_type(a_sdp, "typ host"), count_type(a_sdp, "typ srflx"),
           count_type(a_sdp, "typ relay"));
    CHECK(count_type(a_sdp, "typ host") > 0, "A 至少有 1 个 host 候选");

    // ---- 交换候选（模拟经信令） ----
    printf("\n=== 2. 交换候选并做连通性检查 ===\n");
    CHECK(b.set_remote_sdp(a_sdp), "B 设置 A 的候选");
    CHECK(a.set_remote_sdp(b_sdp), "A 设置 B 的候选");

    const bool a_conn = a.wait_connected(10000);
    const bool b_conn = b.wait_connected(10000);
    CHECK(a_conn, "A 选出可用候选对");
    CHECK(b_conn, "B 选出可用候选对");
    printf("    A 选中候选对: %s\n", a.selected_pair().c_str());
    printf("    B 选中候选对: %s\n", b.selected_pair().c_str());

    // ---- 双向传输媒体（真正的穿透验证） ----
    printf("\n=== 3. 双向媒体传输（%d 秒）===\n", seconds);

    std::atomic<bool> stop{false};
    // A -> B 发送线程
    std::thread sender_a([&] {
        int seq = 0;
        while (!stop && !a.connected()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (!stop) {
            // 构造一个可识别的载荷：来自 A 的序号
            uint8_t buf[64];
            snprintf(reinterpret_cast<char*>(buf), sizeof(buf), "PKT_FROM_A seq=%d", seq++);
            a.send(buf, strlen(reinterpret_cast<char*>(buf)) + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // B -> A 发送线程
    std::thread sender_b([&] {
        int seq = 0;
        while (!stop && !b.connected()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (!stop) {
            uint8_t buf[64];
            snprintf(reinterpret_cast<char*>(buf), sizeof(buf), "PKT_FROM_B seq=%d", seq++);
            b.send(buf, strlen(reinterpret_cast<char*>(buf)) + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    sender_a.join();
    sender_b.join();

    printf("    A 收到 %d 包 / %d 字节，最近内容: %s\n",
           a_recv.load(), a_bytes.load(),
           a_got.empty() ? "(无)" : reinterpret_cast<char*>(a_got.data()));
    printf("    B 收到 %d 包 / %d 字节，最近内容: %s\n",
           b_recv.load(), b_bytes.load(),
           b_got.empty() ? "(无)" : reinterpret_cast<char*>(b_got.data()));

    CHECK(a_recv > 0, "A 收到来自 B 的媒体数据（穿透成功）");
    CHECK(b_recv > 0, "B 收到来自 A 的媒体数据（穿透成功）");
    CHECK(a.tx_count() > 0 && b.tx_count() > 0, "双向都有发送");

    // 验证收到的内容确实来自对端
    CHECK(a_got.size() > 0 && memcmp(a_got.data(), "PKT_FROM_B", 10) == 0,
          "A 收到的数据内容确实来自 B");
    CHECK(b_got.size() > 0 && memcmp(b_got.data(), "PKT_FROM_A", 10) == 0,
          "B 收到的数据内容确实来自 A");

    a.close();
    b.close();

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
