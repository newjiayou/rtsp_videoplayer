#include "logger.h"

#include <chrono>
#include <iostream>

namespace edgelive {

namespace {
const char* ToString(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}
} // namespace

void Log(LogLevel level, const std::string& tag, const std::string& message) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::cout << "[" << now << "] [" << ToString(level) << "] [" << tag << "] " << message << std::endl;
}

} // namespace edgelive

