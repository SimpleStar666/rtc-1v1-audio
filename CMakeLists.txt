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

# ---------- 加密模块（DTLS-SRTP）----------
add_library(rtc_crypto STATIC
  src/crypto/dtls_srtp.cpp
)
target_include_directories(rtc_crypto PUBLIC src)
target_link_libraries(rtc_crypto PUBLIC rtc_common ${SRTP2_LIBRARIES} ${OPENSSL_LIBRARIES})
target_include_directories(rtc_crypto PRIVATE ${SRTP2_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})

# ---------- 网络抗损模块（NACK / FEC / 拥塞控制）----------
add_library(rtc_net STATIC
  src/net/loss_protection.cpp
)
target_include_directories(rtc_net PUBLIC src)
target_link_libraries(rtc_net PUBLIC rtc_common)

# ---------- 指标统计 ----------
add_library(rtc_metrics STATIC
  src/metrics/metrics.cpp
)
target_include_directories(rtc_metrics PUBLIC src)
target_link_libraries(rtc_metrics PUBLIC rtc_common)

# ---------- 可执行目标（按阶段逐个加入）----------
set(RTC_EXE_LINK rtc_common rtc_media rtc_protocol rtc_signal rtc_ice rtc_crypto rtc_net rtc_metrics pthread)

add_executable(selftest tests/selftest.cpp)
target_link_libraries(selftest PRIVATE ${RTC_EXE_LINK} ${OPUS_LIBRARIES} ${SRTP2_LIBRARIES})

add_executable(signal_server src/signal/signal_server.cpp)
target_link_libraries(signal_server PRIVATE ${RTC_EXE_LINK} ${LWS_LIBRARIES} ${GLIB_LIBRARIES})
target_include_directories(signal_server PRIVATE ${LWS_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})

add_executable(peer src/app/peer.cpp src/app/call_session.cpp)
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

add_executable(dtls_srtp_test tests/dtls_srtp_test.cpp)
target_link_libraries(dtls_srtp_test PRIVATE ${RTC_EXE_LINK} ${SRTP2_LIBRARIES} ${OPENSSL_LIBRARIES})
target_include_directories(dtls_srtp_test PRIVATE ${SRTP2_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})

add_executable(loss_protection_test tests/loss_protection_test.cpp)
target_link_libraries(loss_protection_test PRIVATE ${RTC_EXE_LINK})

# 端到端集成测试：音频经 ICE + DTLS-SRTP 完整链路
add_executable(e2e_test tests/e2e_test.cpp)
target_link_libraries(e2e_test PRIVATE ${RTC_EXE_LINK}
  ${OPUS_LIBRARIES} ${SRTP2_LIBRARIES} ${NICE_LIBRARIES}
  ${GLIB_LIBRARIES} ${OPENSSL_LIBRARIES})
target_include_directories(e2e_test PRIVATE
  ${OPUS_INCLUDE_DIRS} ${SRTP2_INCLUDE_DIRS} ${NICE_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})

add_executable(metrics_test tests/metrics_test.cpp)
target_link_libraries(metrics_test PRIVATE ${RTC_EXE_LINK})
