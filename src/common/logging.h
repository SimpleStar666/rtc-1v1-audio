#pragma once
// 轻量日志：带级别、时间戳、线程安全
#include <cstdio>
#include <cstdarg>
#include <string>

namespace rtc {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError };

void set_log_level(LogLevel lv);
LogLevel log_level();
void log_write(LogLevel lv, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// 当前毫秒级单调时间（用于抖动统计、超时判断）
int64_t now_ms();

}  // namespace rtc

#define LOGD(tag, ...) ::rtc::log_write(::rtc::LogLevel::kDebug, tag, __VA_ARGS__)
#define LOGI(tag, ...) ::rtc::log_write(::rtc::LogLevel::kInfo, tag, __VA_ARGS__)
#define LOGW(tag, ...) ::rtc::log_write(::rtc::LogLevel::kWarn, tag, __VA_ARGS__)
#define LOGE(tag, ...) ::rtc::log_write(::rtc::LogLevel::kError, tag, __VA_ARGS__)
