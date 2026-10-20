#!/usr/bin/env python3
# 一次性给 C 盘 CMakeLists 加全部视频相关配置（幂等）
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/CMakeLists.txt'
s = open(P, encoding='utf-8').read()
n = 0

def rep(old, new, label):
    global s, n
    if new.split('\n')[0] in s and old not in s:
        print(f'  - {label}（已存在）')
        return
    if old not in s:
        print(f'  ✗ {label}：找不到锚点')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# 1) 依赖
rep('pkg_check_modules(OPENSSL REQUIRED openssl)',
    'pkg_check_modules(OPENSSL REQUIRED openssl)\n'
    '# VP8：WebRTC 强制实现的视频编解码器（RFC 7742），无专利授权问题\n'
    'pkg_check_modules(VPX REQUIRED vpx)',
    'pkg_check_modules(VPX)')

rep('message(STATUS "openssl     : ${OPENSSL_VERSION}")',
    'message(STATUS "openssl     : ${OPENSSL_VERSION}")\n'
    'message(STATUS "libvpx      : ${VPX_VERSION}")',
    '版本打印')

# 2) rtc_media 源文件 + 链接
rep("""add_library(rtc_media STATIC
  src/media/opus_codec.cpp
  src/media/jitter_buffer.cpp
  src/media/virtual_device.cpp
  src/media/audio_pipeline.cpp
)""",
"""add_library(rtc_media STATIC
  src/media/opus_codec.cpp
  src/media/jitter_buffer.cpp
  src/media/virtual_device.cpp
  src/media/audio_pipeline.cpp
  src/media/vp8_codec.cpp
  src/media/video_device.cpp
  src/media/video_pipeline.cpp
)""",
'rtc_media 加视频源文件')

rep('target_link_libraries(rtc_media PUBLIC rtc_common ${OPUS_LIBRARIES})',
    'target_link_libraries(rtc_media PUBLIC rtc_common ${OPUS_LIBRARIES} ${VPX_LIBRARIES})',
    'rtc_media 链接 vpx')

rep('target_include_directories(rtc_media PRIVATE ${OPUS_INCLUDE_DIRS})',
    'target_include_directories(rtc_media PRIVATE ${OPUS_INCLUDE_DIRS} ${VPX_INCLUDE_DIRS})',
    'rtc_media include vpx')

# 3) 测试目标
rep('add_executable(weaknet_test tests/weaknet_test.cpp)',
    """add_executable(vp8_test tests/vp8_test.cpp)
target_link_libraries(vp8_test PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES} ${VPX_LIBRARIES})
target_include_directories(vp8_test PRIVATE ${OPUS_INCLUDE_DIRS} ${VPX_INCLUDE_DIRS})

add_executable(video_test tests/video_test.cpp)
target_link_libraries(video_test PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES} ${VPX_LIBRARIES})
target_include_directories(video_test PRIVATE ${OPUS_INCLUDE_DIRS} ${VPX_INCLUDE_DIRS})

add_executable(weaknet_test tests/weaknet_test.cpp)""",
    'vp8_test / video_test 目标')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 共 {n} 处')
print('\n=== 校验 ===')
c = open(P, encoding='utf-8').read()
for k in ['VPX', 'vp8_codec', 'video_device', 'video_pipeline', 'vp8_test', 'video_test']:
    print(f'  {k:16s}: {c.count(k)} 次')
