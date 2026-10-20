#!/usr/bin/env python3
# 给 CMakeLists 加 libvpx 依赖与 vp8 源文件
P = '/home/ts/rtc-1v1-audio/CMakeLists.txt'
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

# 1) 依赖探测
rep('pkg_check_modules(OPENSSL REQUIRED openssl)',
    'pkg_check_modules(OPENSSL REQUIRED openssl)\n'
    '# VP8：WebRTC 强制实现的视频编解码器（RFC 7742），无专利授权问题\n'
    'pkg_check_modules(VPX REQUIRED vpx)',
    'pkg_check_modules(VPX)')

rep('message(STATUS "openssl     : ${OPENSSL_VERSION}")',
    'message(STATUS "openssl     : ${OPENSSL_VERSION}")\n'
    'message(STATUS "libvpx      : ${VPX_VERSION}")',
    '版本打印')

# 2) rtc_media 链接 vpx
rep('target_link_libraries(rtc_media PUBLIC rtc_common ${OPUS_LIBRARIES})',
    'target_link_libraries(rtc_media PUBLIC rtc_common ${OPUS_LIBRARIES} ${VPX_LIBRARIES})',
    'rtc_media 链接 vpx')

rep('target_include_directories(rtc_media PRIVATE ${OPUS_INCLUDE_DIRS})',
    'target_include_directories(rtc_media PRIVATE ${OPUS_INCLUDE_DIRS} ${VPX_INCLUDE_DIRS})',
    'rtc_media include vpx')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 共 {n} 处')

# 显示 rtc_media 区块
print('\n=== rtc_media 区块 ===')
lines = s.split('\n')
for i, ln in enumerate(lines):
    if 'add_library(rtc_media' in ln:
        for j in range(i, min(i + 12, len(lines))):
            print(f'  {j+1}: {lines[j]}')
        break
