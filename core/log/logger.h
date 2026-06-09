#pragma once

#include <string>

namespace edgelive {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

void Log(LogLevel level, const std::string& tag, const std::string& message);

} // namespace edgelive

#define EDGELIVE_LOG_TRACE(tag, msg) ::edgelive::Log(::edgelive::LogLevel::Trace, tag, msg)
#define EDGELIVE_LOG_DEBUG(tag, msg) ::edgelive::Log(::edgelive::LogLevel::Debug, tag, msg)
#define EDGELIVE_LOG_INFO(tag, msg) ::edgelive::Log(::edgelive::LogLevel::Info, tag, msg)
#define EDGELIVE_LOG_WARN(tag, msg) ::edgelive::Log(::edgelive::LogLevel::Warn, tag, msg)
#define EDGELIVE_LOG_ERROR(tag, msg) ::edgelive::Log(::edgelive::LogLevel::Error, tag, msg)

