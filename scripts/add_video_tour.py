#!/usr/bin/env python3
# 在代码导读里插入"模块 11：视频"，并更新顶部说明
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/05-code-tour.md'
s = open(P, encoding='utf-8').read()

VIDEO = """## 模块 11：视频（VP8 编解码 + 分片 + 关键帧请求）—— `src/media/vp8_codec.*` / `video_device.*` / `video_pipeline.*`

> 音频链路是"一帧一个包、丢了 PLC 补"；**视频完全是另一套逻辑**。
> 这一节讲清视频的四个独特问题：**帧太大要分片、丢片整帧作废、
> 丢关键帧永久卡死、必须请求关键帧**。

### 为什么选 VP8 而不是 H.264

| 理由 | 说明 |
|---|---|
| **WebRTC 强制实现** | RFC 7742 规定 VP8 是 WebRTC 的必选视频编解码器，H.264 是可选 |
| 无专利授权 | H.264/H.265 有 MPEG-LA 专利池，VP8 无此问题 |
| 有 SVC 扩展路径 | WebRTC 的 VP8 SVC / simulcast 都基于它 |
| 纯 C API | libvpx 接口清晰，容易封装 |

### 代码在哪

| 功能 | 位置 |
|---|---|
| `YuvFrame`（I420 图像） | `vp8_codec.h:29-42` |
| **编码器初始化**（CBR/关键帧间隔/线程） | `vp8_codec.cpp:60` |
| **`encode()` 编码一帧** | `vp8_codec.cpp:120` |
| **`request_keyframe()` 强制下一帧关键帧** | `vp8_codec.cpp:112` |
| **`decode()` 解码 + stride 逐行拷贝** | `vp8_codec.cpp:235` |
| **`set_bitrate()` 运行时改码率** | `vp8_codec.cpp:190` |
| 虚拟视频设备（采集图案 / PPM 落盘） | `video_device.cpp` |
| **`VideoFragmentHeader` 分片头** | `video_pipeline.h:56-66` |
| **发送：编码 → 分片 → 逐片发** | `video_pipeline.cpp:95` |
| **接收：分片重组** | `video_pipeline.cpp:200` |
| **`request_keyframe_throttled()` PLI 限流** | `video_pipeline.cpp:30` |
| **关键帧丢失判断 + 请求** | `video_pipeline.cpp:225` |

### 视频与音频的四个本质差异

| 维度 | 音频（Opus） | 视频（VP8） |
|---|---|---|
| 单帧大小 | 40~120 字节 | **2000~8000 字节** |
| 一帧一包？ | 可以 | **不行，必须分片**（MTU 约 1200） |
| 丢包后果 | PLC 补一帧，几乎无感 | **整帧作废**（缺一片就拼不出） |
| 丢关键帧后果 | 无特殊影响 | **后续所有 P 帧都无法解码** |
| 恢复手段 | 本地 PLC（不需要对端） | **PLI 请求关键帧（要 1 个 RTT）** |
| 码率量级 | 16 kbps | **400 kbps**（高 25 倍） |

### 关键实现 ①：分片与重组

**发送端**：一帧切成 N 片，每片加 8 字节头

```cpp
// video_pipeline.h:56
struct VideoFragmentHeader {
    uint16_t frame_id;     // 哪一帧
    uint16_t frag_index;   // 帧内第几片
    uint16_t frag_count;   // 这帧共几片
    bool     is_keyframe;  // 是否关键帧
};  // 8 字节

// video_pipeline.cpp:95 发送
const uint16_t frag_count = (total + max_payload - 1) / max_payload;  // 1100B/片
for (uint16_t i = 0; i < frag_count; i++) {
    // 载荷 = 分片头 + 该片数据
    // PacketHeader.marker = (i == frag_count - 1)   ← RTP M 位标记帧结束
}
```

**接收端**：按 frame_id 聚合，凑齐才拼

```cpp
// video_pipeline.cpp:200
auto& pf = pending_[frame_id];
pf.got.insert(frag_index);              // 记录收到了哪些片
if (pf.got.size() == pf.frag_count) {   // 凑齐了
    complete = true;
}
```

**关键**：缺任何一片 → 整帧丢弃（**不是花屏**）。
实测：15% 分片丢失 → 22 帧被丢弃，37 帧成功解码。

### 关键实现 ②：关键帧请求（PLI）—— 视频独有

**为什么必须有**：视频是帧间预测编码

```
I 帧（关键帧）：独立编码，可单独解码
P 帧：只编码"与参考帧的差异"，必须知道参考帧
     → 链头（关键帧）断了，整条链都废
```

实测（`vp8_test` 第 4 组）：
```
从关键帧开始解码: 6/6 成功
丢弃关键帧后解码 P 帧: 0/5 成功      ← 全失败，且不会自愈
```

**机制**：

```cpp
// 接收端：解码失败 / 关键帧缺片 → 请求（video_pipeline.cpp:30）
void VideoPipeline::request_keyframe_throttled() {
    const int64_t now = now_ms();
    // 0 = 从未请求过；否则限流 300ms
    if (last_kf_req_ms_ != 0 && now - last_kf_req_ms_ < 300) return;
    last_kf_req_ms_ = now;
    kf_req_fn_();       // → 走信令发 kKeyframeRequest 给对端
}

// 发送端：收到请求 → 下一帧强制关键帧（vp8_codec.cpp:112）
void Vp8Codec::request_keyframe() { force_keyframe_ = true; }
// encode() 时用 VPX_EFLAG_FORCE_KF
```

**两个必须做的细节**：

| 细节 | 为什么 |
|---|---|
| **必须限流**（300ms） | 否则网络差时形成"请求风暴"，反而挤占带宽 |
| **触发路径要全** | 解码失败要请求；**关键帧缺片被丢弃也要请求**（否则高丢包时永远不请求） |

第二条是坑 44 的教训：第一版只在"解码失败"时请求，
但高丢包时帧根本到不了解码器（缺片在重组阶段就丢了）→ 一次都不请求 → 画面永久卡死。

### 关键实现 ③：周期性关键帧

即使没有 PLI，发送端也必须**周期性**发关键帧：

```cpp
ecfg.kf_mode = VPX_KF_AUTO;
ecfg.kf_max_dist = cfg_.keyframe_interval;   // 本项目 = fps*2，即每 2 秒
```

为什么：新加入的接收端、或长时间丢包后，都需要一个"可独立解码的起点"。
WebRTC 的默认关键帧间隔通常是 2~10 秒（越短抗损越好，但码率越高）。

### 关键实现 ④：YUV420 与 stride

```cpp
// 解码时不能整块 memcpy —— stride 可能大于宽度（编码器会对齐到 16/32 像素）
for (int r = 0; r < h; r++) {
    memcpy(out->y.data() + r * w,
           img->planes[VPX_PLANE_Y] + r * img->stride[VPX_PLANE_Y], w);
}
```

**YUV420 内存布局**：Y 全分辨率（w×h），U/V 各 1/4（w/2 × h/2）。
总大小 = w×h×1.5。这是视频处理的事实标准格式（libvpx/x264/ffmpeg 都支持）。

### 实测数据

**VP8 编解码**（`vp8_test`，320x240 @15fps）：
```
编码 30 帧，解码成功 30 帧
关键帧: 2 个，关键帧平均 7127 字节
非关键帧平均: 2121 字节          ← 关键帧是 P 帧的 3.4 倍
总码流: 73669 字节，平均码率 294.7 kbps
平均 PSNR: 52.02 dB              ← 视频质量很好（>35 就算优秀）
```

**视频管线**（`video_test`）：
```
无丢包:  A 发 138 分片 → B 重组 46 帧 → 解码 46 帧，丢弃 0
15%丢片: A 发 188 分片 → B 收 163 片 → 完整 37 帧，丢弃 22 帧
丢关键帧: B 请求 PLI 1 次 → 恢复后解码 45 帧（自愈成功）
限流验证: 4 秒内 7 次请求（1.8 次/秒，未超 300ms 间隔上限）
```

**不同内容的编码效率**（同 800kbps 目标）：
```
彩条（大面积平坦）      实测   18 kbps    ← 极易压缩
渐变+运动（接近真实）   实测  562 kbps
噪声（最难压缩）        实测 3835 kbps    ← 编码器被迫用更多比特
```

**端到端音视频通话**（双进程）：
```
A 端: 编码 123 帧 / 解码 122 帧 / 丢弃 0 / 实测 31 kbps（彩条源）
B 端: 编码 122 帧 / 解码 123 帧 / 丢弃 0 / 实测 423 kbps（运动源）
A 端收到 122 帧 PPM，B 端收到 122 帧 PPM
```

**画面交叉验证**（最硬的证据）：

| 端点 | 采集源 | 收到的画面 | 颜色数 |
|---|---|---|---|
| A | 彩条（8 色带） | B 的运动图案 | 5397 |
| B | 运动图案 | **A 的彩条** | **8~12** |

B 收到的画面只有 8 种颜色 —— 正是彩条的色带数。
**这就是"两端真的在互传视频"的证明**（不是自环）。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 编解码器 | VP8（libvpx） | 压缩率不如 H.265 | WebRTC 必选项，且无专利问题 |
| 分片大小 | 1100 字节 | 略保守（浪费一点带宽） | 超过 MTU → IP 分片 → 丢一片废整包 |
| 缺片处理 | **整帧丢弃** | 卡顿一下 | 花屏（解出残缺画面）更难看 |
| 关键帧间隔 | 2 秒 | 码率略高 | 新接收端要等很久才能出画面 |
| PLI 限流 | 300ms | 恢复略慢 | 请求风暴挤占带宽 |
| PLI 通道 | 信令（TCP） | 多一跳 | 走 UDP 可能丢，请求本身丢失 |
| 单帧单包？ | 否，分片 | 复杂度上升 | 关键帧根本发不出去（坑 42） |

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **42** | 视频一帧一包超过 MTU，关键帧几乎收不到 | 分片（1100B/片）+ 分片头 |
| **43** ⭐ | 丢关键帧后后续帧全不可解码，画面永久卡死 | PLI 请求关键帧 + 周期性关键帧 |
| **44** ⭐ | PLI 限流基准 + 触发路径不全（高丢包时不请求） | 统一 `request_keyframe_throttled()` + 缺片路径也触发 |

### 这一节最值得记住的一句话

> **视频的"丢包恢复"不是补数据，而是"请求重发一个可独立解码的起点"。**
> 因为 P 帧依赖参考帧，本地无法凭空生成画面（不像音频能 PLC 猜）。

---

"""

old = '## 跨模块的三个核心设计'
if old not in s:
    raise SystemExit('❌ 找不到插入点')
s = s.replace(old, VIDEO + old, 1)

# 顶部说明更新
s = s.replace(
'> **已同步修复**：坑 39（JitterBuffer 加锁）/ 40（FEC 长度 u8→u16）/ '
'41（目标码率接口真正生效）—— 本文档中标注为"边界/未实现"的三处已全部修复。',
'> **已同步修复**：坑 39（JitterBuffer 加锁）/ 40（FEC 长度 u8→u16）/ '
'41（目标码率接口真正生效）—— 标注为"边界/未实现"的三处已全部修复。\n'
'> **视频已加入**：模块 11 讲 VP8 编解码、分片重组、关键帧请求（PLI）。\n'
'> 项目现在是**完整的音视频 RTC**，不只是音频。', 1)

open(P, 'w', encoding='utf-8').write(s)
print('✅ 已插入模块 11：视频')
print('   模块数:', s.count('\n## 模块'))
print('   总行数:', s.count('\n') + 1)
