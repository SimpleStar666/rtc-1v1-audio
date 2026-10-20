#!/usr/bin/env python3
# 把 vp8_codec.cpp 加入 rtc_media，并新增 vp8_test 目标
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

# 在 weaknet_test 之后加 vp8_test / video_test 目标
anchor = """add_executable(weaknet_test tests/weaknet_test.cpp)"""
if anchor in s:
    s = s.replace(anchor,
"""add_executable(vp8_test tests/vp8_test.cpp)
target_link_libraries(vp8_test PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES} ${VPX_LIBRARIES})
target_include_directories(vp8_test PRIVATE ${OPUS_INCLUDE_DIRS} ${VPX_INCLUDE_DIRS})

add_executable(video_test tests/video_test.cpp)
target_link_libraries(video_test PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES} ${VPX_LIBRARIES})
target_include_directories(video_test PRIVATE ${OPUS_INCLUDE_DIRS} ${VPX_INCLUDE_DIRS})

""" + anchor, 1)
    print('  ✓ 新增 vp8_test / video_test 目标')
    n += 1

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 共 {n} 处')
