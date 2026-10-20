#!/usr/bin/env python3
# 更新代码导读里"边界提醒"——这三处已经修复
import re, sys

P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/05-code-tour.md'
lines = open(P, encoding='utf-8').read().split('\n')

print('=== 搜索待更新的边界提醒 ===')
keys = ['只是日志占位', 'u8', '无锁', '截断', 'JitterBuffer 内部', '线程', 'mutex']
for i, ln in enumerate(lines):
    for k in keys:
        if k in ln and len(ln.strip()) > 8:
            print(f'  行 {i+1}: {ln[:150]}')
            break
