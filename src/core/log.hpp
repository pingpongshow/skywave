// Leveled logging (CONFIGURATION.md: 0=TRACE .. 4=ERROR).
#pragma once

#include <cstdarg>
#include <string>

namespace skw {

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

namespace log {
void setLevel(LogLevel lvl);
// Empty path closes any open log file (stderr only).
bool setFile(const std::string& path);
void write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
}  // namespace log

#define SKW_TRACE(...) ::skw::log::write(::skw::LogLevel::Trace, __VA_ARGS__)
#define SKW_DEBUG(...) ::skw::log::write(::skw::LogLevel::Debug, __VA_ARGS__)
#define SKW_INFO(...)  ::skw::log::write(::skw::LogLevel::Info, __VA_ARGS__)
#define SKW_WARN(...)  ::skw::log::write(::skw::LogLevel::Warn, __VA_ARGS__)
#define SKW_ERROR(...) ::skw::log::write(::skw::LogLevel::Error, __VA_ARGS__)

}  // namespace skw
