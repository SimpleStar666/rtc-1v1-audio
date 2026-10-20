#!/usr/bin/env python3
# 通过微调行距/段后间距压回 2 页（优先不动内容）
from docx import Document
from docx.shared import Pt, Emu

path = r"C:\Users\小陶\Desktop\陶云飞-WebRTC音视频开发工程师.docx"
d = Document(path)

# 目标：
#   行距 0.958 -> 0.92（约省 4%）
#   段后 1pt -> 0.6pt
#   段前 2pt -> 1.5pt（小标题的段前）
# 这样不动一个字，纯排版挤出约 5-8 行

n = 0
for p in d.paragraphs:
    t = p.text.strip()
    if not t:
        continue
    pf = p.paragraph_format
    # 行距
    if pf.line_spacing and pf.line_spacing > 0.90:
        pf.line_spacing = 0.92
        n += 1
    # 段后
    if pf.space_after and pf.space_after > Pt(0.5):
        pf.space_after = Pt(0.5)
    # 段前（只有小标题有 2pt）
    if pf.space_before and pf.space_before > Pt(1):
        pf.space_before = Pt(1.2)

d.save(path)
print(f'已调整 {n} 段的行距为 0.92，段后 0.5pt，段前 1.2pt')
