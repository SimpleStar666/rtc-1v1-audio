#!/usr/bin/env python3
# 补 04-how-to-run.md：视频相关运行说明
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/04-how-to-run.md'
s = open(P, encoding='utf-8').read()
n = 0

def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# ---- 1) 顶部定位 ----
rep('> 面向"想实际跑起来"的人。所有命令都在 WSL 中实测通过，输出可直接对照。',
    '> 面向"想实际跑起来"的人。所有命令都在 WSL 中实测通过，输出可直接对照。\n>\n'
    '> **本项目是音视频通话**：默认同时传 Opus 音频 + VP8 视频。\n'
    '> 只想跑音频可以加 `--no-video`。',
    '顶部定位')

# ---- 2) 在"听到声音"之前插入"看到画面"章节 ----
VIDEO_SECTION = """---

## 6.5 **看到画面**（视频验证）

视频和音频一样能"肉眼验证"——解码出的帧会落盘为 PPM 图片。

### 一条命令

```bash
./scripts/run.sh call
```

它会启动两个端点（A 采彩条、B 采运动图案），通话结束后：

```
=== 视频传输 ===
  A 端: 编码 123 / 123    解码/丢弃 122 / 0      实测 31 kbps
  B 端: 编码 122 / 122    解码/丢弃 123 / 0      实测 423 kbps
  解码帧落盘: A 端收到 122 帧 / B 端收到 122 帧（PPM）
  ✓ 视频双向传输成功
```

### 看画面

```bash
# 解码出的帧在 /tmp/vsink_A 和 /tmp/vsink_B
ls /tmp/vsink_A/*.ppm | head

# PPM 是未压缩图像，Windows 可能不直接支持，转成 PNG：
python3 - <<'EOF'
import struct, zlib
def ppm2png(src, dst):
    f = open(src,'rb'); assert f.readline().strip()==b'P6'
    line = f.readline()
    while line.startswith(b'#'): line = f.readline()
    w,h = map(int, line.split()); f.readline()
    rgb = f.read(w*h*3); f.close()
    raw = b''.join(b'\\x00'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t,d):
        c = t+d
        return struct.pack('>I',len(d))+c+struct.pack('>I',zlib.crc32(c)&0xffffffff)
    png = b'\\x89PNG\\r\\n\\x1a\\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
    png += chunk(b'IDAT', zlib.compress(raw,6))
    png += chunk(b'IEND', b'')
    open(dst,'wb').write(png)
ppm2png('/tmp/vsink_A/out_0060.ppm', '/tmp/frame_A.png')
ppm2png('/tmp/vsink_B/out_0060.ppm', '/tmp/frame_B.png')
print('OK')
EOF
```

然后打开 `/tmp/frame_A.png` 和 `/tmp/frame_B.png`。

### 期望看到什么

| 文件 | 采集源 | 你看到的画面 |
|---|---|---|
| `/tmp/frame_A.png` | A 采**彩条** | **B 的运动图案**（渐变底 + 白方块 + 黑圆） |
| `/tmp/frame_B.png` | B 采运动图案 | **A 的彩条**（8 条竖直色带） |

**这是交叉的** —— A 收到的是 B 的画面，B 收到的是 A 的画面。
如果看到自己的画面，说明是自环而非真通话。

### 视频参数

```bash
./build/peer --call --signal ws://127.0.0.1:8899 --room demo --name A \\
  --video-size 640x480      # 分辨率（默认 320x240）
  --video-fps 30            # 帧率（默认 15）
  --video-kbps 800          # 视频码率（默认 400）
  --video-source test:bars  # 画面源（见下）
  --video-sink /tmp/frames  # 解码帧落盘目录（不填则只统计不落盘）
```

**三种测试画面源**：

| 源 | 内容 | 适合验证 |
|---|---|---|
| `test:motion`（默认） | 渐变 + 移动方块 + 移动圆 | 最接近真实视频，码率约 400~560kbps |
| `test:bars` | 8 条彩条 | 大面积平坦，极易压缩（仅 ~18kbps） |
| `test:noise` | 随机噪声 | 最难压缩（~3800kbps），压力测试 |

### 只看视频，不看音频

```bash
./build/peer --call --signal ws://127.0.0.1:8899 --room demo --name A \\
  --seconds 5 --video-sink /tmp/frames
```

### 关闭视频

```bash
./scripts/run.sh call     # 音视频（默认）
./build/peer --call ... --no-video    # 纯音频
```

### 视频测试单跑

```bash
cd build
./vp8_test      # VP8 编解码：PSNR、关键帧、码率调整
./video_test    # 视频管线：分片重组、丢包、关键帧请求（PLI）
```

### 如果画面是花屏/卡顿

| 现象 | 原因 | 排查 |
|---|---|---|
| 完全没画面，`丢弃` 很多 | 丢关键帧后没收到新关键帧 | 看日志 `关键帧请求` 次数；确认信令服务器在转发 |
| 画面卡在某一帧 | 参考帧链断了 | 同上，PLI 机制 |
| 花屏但继续动 | 分片重组错乱 | 看 `接收分片/完整帧` 是否对得上 |
| 码率远高于目标 | 画面太复杂（噪声源） | 换 `test:motion` 或降分辨率 |

---

"""

anchor = '---\n\n## 7. 双端真实通话演示'
if anchor not in s:
    # 尝试另一个锚点
    anchor = '## 7. 双端真实通话演示'
if anchor not in s:
    print('  ✗ 找不到插入点')
else:
    s = s.replace(anchor, VIDEO_SECTION + anchor.lstrip('-\n'), 1)
    print('  ✓ 插入"看到画面"章节')
    n += 1

# ---- 3) 命令速查表加视频 ----
rep('''./build/e2e_test --seconds 4                      # 完整链路（加密+ICE）''',
    '''./build/e2e_test --seconds 4                      # 完整链路（加密+ICE）
./build/vp8_test                                  # VP8 编解码（PSNR/关键帧/码率）
./build/video_test                                # 视频管线（分片/重组/PLI）''',
    '命令速查加视频测试')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 04-how-to-run 共 {n} 处，现 {s.count(chr(10))+1} 行')
