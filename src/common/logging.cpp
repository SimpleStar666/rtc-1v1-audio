#include "common/logging.h"

#include <chrono>
#include <mutex>

namespace rtc {

namespace {
std::mutex g_log_mutex;
LogLevel   g_level = LogLevel::kInfo;

const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo:  return "INFO ";
        case LogLevel::kWarn:  return "WARN ";
        case LogLevel::kError: return "ERROR";
    }
    return "?????";
}
}  // namespace

void set_log_level(LogLevel lv) { g_level = lv; }
LogLevel log_level() { return g_level; }

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void log_write(LogLevel lv, const char* tag, const char* fmt, ...) {
    if (static_cast<int>(lv) < static_cast<int>(g_level)) return;

    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stderr, "[%8lld][%s][%s] %s\n",
            static_cast<long long>(now_ms()), level_name(lv), tag ? tag : "-", body);
}

}  // namespace rtc
