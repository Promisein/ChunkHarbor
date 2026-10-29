#include "chunkharbor/common/Logger.h"

#include <iostream>
#include <mutex>

namespace chunkharbor {

void log(LogLevel level, std::string_view message) {
    static std::mutex mutex;
    const char* name = "INFO";
    if (level == LogLevel::Warning) name = "WARN";
    if (level == LogLevel::Error) name = "ERROR";
    std::lock_guard<std::mutex> lock(mutex);
    std::cerr << '[' << name << "] " << message << '\n';
}

} // namespace chunkharbor
