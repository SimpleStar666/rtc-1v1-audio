#!/usr/bin/env python3
# 精确定位：第 3 页从哪开始（用 Word COM 逐段查页码）
import win32com.client
import io

path = r"C:\Users\小陶\Desktop\陶云飞-WebRTC音视频开发工程师.docx"
word = win32com.client.Dispatch("Word.Application")
word.Visible = False
word.DisplayAlerts = 0
doc = word.Documents.Open(path, False, True)

lines = []
lines.append(f'总页数: {doc.ComputeStatistics(2)}')
lines.append('')
lines.append('=== 每段所在页码 ===')
# wdActiveEndPageNumber = 3
for i in range(1, doc.Paragraphs.Count + 1):
    p = doc.Paragraphs(i)
    txt = p.Range.Text.strip()
    if not txt:
        continue
    page = p.Range.Information(3)   # wdActiveEndPageNumber
    lines.append(f'  [p{page}] {txt[:70]}')

doc.Close(False)
try: word.Quit()
except: pass

out = r"C:\Users\小陶\Documents\deepseek-harness\default-workspace\page_break.txt"
with io.open(out, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines))
print(f'OK -> {out}')
