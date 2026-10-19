cmake_minimum_required(VERSION 3.21)
project(rtc_1v1_audio CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

# ---------- 依赖 ----------
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS   REQUIRED opus)
pkg_check_modules(SRTP2  REQUIRED libsrtp2)
pkg_check_modules(NICE   REQUIRED nice)
pkg_check_modules(LWS    REQUIRED libwebsockets)
pkg_check_modules(GLIB   REQUIRED glib-2.0)
pkg_check_modules(OPENSSL REQUIRED openssl)

message(STATUS "opus        : ${OPUS_VERSION}")
message(STATUS "libsrtp2    : ${SRTP2_VERSION}")
message(STATUS "libnice     : ${NICE_VERSION}")
message(STATUS "libwebsockets: ${LWS_VERSION}")
message(STATUS "openssl     : ${OPENSSL_VERSION}")

# ---------- 公共库 ----------
add_library(rtc_common STATIC
  src/common/logging.cpp
  src/common/util.cpp
)
target_include_directories(rtc_common PUBLIC src)
target_link_libraries(rtc_common PUBLIC pthread)

# ---------- 阶段 2：音频（Opus + 抖动缓冲 + 虚拟设备）----------
add_library(rtc_media STATIC
  src/media/opus_codec.cpp
  src/media/jitter_buffer.cpp
  src/media/virtual_device.cpp
  src/media/audio_pipeline.cpp
)
target_include_directories(rtc_media PUBLIC src)
target_link_libraries(rtc_media PUBLIC rtc_common ${OPUS_LIBRARIES})
target_include_directories(rtc_media PRIVATE ${OPUS_INCLUDE_DIRS})

# ---------- 阶段 3：协议 ----------
add_library(rtc_protocol STATIC
  src/protocol/packet.cpp
  src/protocol/signaling_msg.cpp
)
target_include_directories(rtc_protocol PUBLIC src)
target_link_libraries(rtc_protocol PUBLIC rtc_common)

# ---------- 信令模块 ----------
add_library(rtc_signal STATIC
  src/signal/signaling_client.cpp
)
target_include_directories(rtc_signal PUBLIC src)
target_link_libraries(rtc_signal PUBLIC rtc_common rtc_protocol ${LWS_LIBRARIES})
target_include_directories(rtc_signal PRIVATE ${LWS_INCLUDE_DIRS})

# ---------- ICE 传输模块 ----------
add_library(rtc_ice STATIC
  src/ice/ice_transport.cpp
)
target_include_directories(rtc_ice PUBLIC src)
target_link_libraries(rtc_ice PUBLIC rtc_common ${NICE_LIBRARIES} ${GLIB_LIBRARIES})
target_include_directories(rtc_ice PRIVATE ${NICE_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})

# ---------- 可执行目标（按阶段逐个加入）----------
set(RTC_EXE_LINK rtc_common rtc_media rtc_protocol rtc_signal rtc_ice pthread)

add_executable(selftest tests/selftest.cpp)
target_link_libraries(selftest PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES} ${SRTP2_LIBRARIES})

add_executable(signal_server src/signal/signal_server.cpp)
target_link_libraries(signal_server PRIVATE ${RTC_EXE_LINK} ${LWS_LIBRARIES} ${GLIB_LIBRARIES})
target_include_directories(signal_server PRIVATE ${LWS_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})

add_executable(peer src/app/peer.cpp)
target_link_libraries(peer PRIVATE ${RTC_EXE_LINK}
  ${OPUS_LIBRARIES} ${SRTP2_LIBRARIES} ${NICE_LIBRARIES} ${LWS_LIBRARIES}
  ${GLIB_LIBRARIES} ${OPENSSL_LIBRARIES})
target_include_directories(peer PRIVATE
  ${OPUS_INCLUDE_DIRS} ${SRTP2_INCLUDE_DIRS} ${NICE_INCLUDE_DIRS}
  ${LWS_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})

add_executable(weaknet_test tests/weaknet_test.cpp)
target_link_libraries(weaknet_test PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES})

add_executable(ice_test tests/ice_test.cpp)
target_link_libraries(ice_test PRIVATE ${RTC_EXE_LINK} ${NICE_LIBRARIES} ${GLIB_LIBRARIES})
target_include_directories(ice_test PRIVATE ${NICE_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})
