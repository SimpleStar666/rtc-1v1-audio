#!/usr/bin/env python3
# 修补遗漏：项目名称 + 去掉 "libwebrtc" 字样
import io, shutil
from docx import Document

path = r"C:\Users\小陶\Desktop\陶云飞-WebRTC音视频开发工程师.docx"
d = Document(path)
paras = d.paragraphs

def set_text(idx, new_text, label):
    p = paras[idx]
    old = p.text.strip()
    if old == new_text:
        print(f'  - [{idx}] {label}: 已相同')
        return False
    if not p.runs:
        p.text = new_text
    else:
        p.runs[0].text = new_text
        for r in p.runs[1:]:
            r.text = ''
    print(f'  [OK] [{idx}] {label}')
    print(f'        旧: {old[:50]}')
    print(f'        新: {new_text[:50]}')
    return True

n = 0

# ---- 遗漏 1：项目名称 [38] ----
# 原："WebRTC 1v1 实时语音通话工具" -> 改为音视频 + 自建
if set_text(38,
    '项目名称：WebRTC 1v1 实时音视频通话系统（自建，不依赖 libwebrtc）',
    '项目名称（补音视频+自建）'):
    n += 1

# ---- 遗漏 2：[40] 去掉 libwebrtc 字样 ----
# 改为正面表述："从零自建" 而不是 "不依赖 libwebrtc"
if set_text(40,
    '项目描述：从零自建的 1v1 实时音视频通话系统（约 1.2 万行 C/C++，'
    '基于 libwebsockets / libnice / libsrtp2 / OpenSSL / libopus / libvpx 六个开源组件）：'
    '终端侧实现 Opus 音频编解码与 VP8 视频编解码、自适应抖动缓冲、RTP 分片重组与关键帧请求；'
    '自建 WebSocket 信令服务完成 SDP 协商与 ICE 候选交换（Trickle ICE），'
    '经 libnice 实现 STUN 打洞与 NAT 穿透，媒体经 DTLS-SRTP 加密传输；'
    '实现 NACK / FEC / GCC 码率自适应对抗弱网。全部链路有可复现的实测数据与 44 项踩坑记录。',
    '项目描述（去掉 libwebrtc 字样）'):
    n += 1

d.save(path)
print(f'\n已保存，共修 {n} 处')

# ---- 复检 ----
d2 = Document(path)
bad = [(i, p.text[:60]) for i, p in enumerate(d2.paragraphs)
       if 'libwebrtc' in p.text.lower() or 'libwebrtc' in p.text]
print(f'\n=== 复检 libwebrtc 残留 ===')
if bad:
    for i, t in bad:
        print(f'  [FAIL] [{i}] {t}')
else:
    print('  [PASS] 已无 libwebrtc')
