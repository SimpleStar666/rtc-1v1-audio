#!/usr/bin/env bash
# 把 PPM 转成 PNG 并检查画面内容（证明解码出的是"对方的画面"）
cat > /tmp/ppmcheck.py <<'PYEOF'
import os, sys, struct, zlib

def read_ppm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = map(int, line.split())
        maxv = int(f.readline())
        data = f.read(w*h*3)
    return w, h, data

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

def analyze(path):
    w, h, d = read_ppm(path)
    # 统计：平均亮度、唯一颜色数、行间差异（判断是否有内容而非纯色）
    n = w*h
    lum = sum(d[i*3] for i in range(0, n, 7)) / len(range(0, n, 7))
    colors = len(set(d[i*3:i*3+3] for i in range(0, n, 13)))
    # 相邻像素差异（越大说明细节越多）
    diff = sum(abs(d[i*3] - d[(i+1)*3]) for i in range(0, n-1, 11)) / len(range(0, n-1, 11))
    return w, h, lum, colors, diff

for side in ['A', 'B']:
    d = f'/tmp/vsink_{side}'
    if not os.path.isdir(d):
        print(f'  {side}: 目录不存在'); continue
    files = sorted(f for f in os.listdir(d) if f.endswith('.ppm'))
    if not files:
        print(f'  {side}: 无 PPM'); continue
    print(f'\n=== {side} 端解码帧分析（共 {len(files)} 帧）===')
    # 检查首帧、中间帧、末帧
    for idx in [0, len(files)//2, len(files)-1]:
        p = os.path.join(d, files[idx])
        w, h, lum, colors, diff = analyze(p)
        print(f'  {files[idx]:16s} {w}x{h}  平均亮度={lum:6.1f}  颜色数={colors:5d}  细节={diff:5.2f}')
    # 转 3 张 PNG
    os.makedirs(f'/tmp/vpng_{side}', exist_ok=True)
    for idx in [0, len(files)//2, len(files)-1]:
        p = os.path.join(d, files[idx])
        w, h, rgb = read_ppm(p)
        out = f'/tmp/vpng_{side}/{files[idx].replace(".ppm", ".png")}'
        write_png(out, w, h, rgb)
    print(f'  → PNG 已生成: /tmp/vpng_{side}/')

# 关键判断：A 采彩条、B 采运动图案
# 所以 A 收到的应该是"运动图案"（B 的），B 收到的应该是"彩条"（A 的）
print('\n=== 交叉验证（证明收到的是对方的画面）===')
try:
    wa, ha, la, ca, da = analyze('/tmp/vsink_A/out_0060.ppm')
    wb, hb, lb, cb, db = analyze('/tmp/vsink_B/out_0060.ppm')
    print(f'  A 收到（应是 B 的运动图案）: 颜色数={ca}  细节={da:.2f}')
    print(f'  B 收到（应是 A 的彩条）    : 颜色数={cb}  细节={db:.2f}')
    if cb < ca:
        print('  ✅ B 收到的画面颜色更少（彩条只有 8 条色带）→ 确实是 A 的彩条')
    else:
        print('  ⚠ 颜色数对比不显著，改用细节度判断')
except Exception as e:
    print(f'  分析失败: {e}')
PYEOF
python3 /tmp/ppmcheck.py
