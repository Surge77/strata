#pragma once

#include "strata/env.hpp"
#include "strata/result.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace strata {

enum class LogLevel : std::uint8_t { kDebug, kInfo, kWarn, kError };

// "DEBUG", "INFO", "WARN", or "ERROR".
[[nodiscard]] std::string_view LogLevelName(LogLevel level) noexcept;

// Where the engine reports what it is doing: opening, recovering, flushing,
// compacting, and anything it had to repair or skip.
//
// Implementations must be safe to call from several threads at once, because
// background work logs concurrently with foreground operations. Logging must
// never fail an operation, so Log returns nothing: a logger that cannot write
// drops the message.
class Logger {
 public:
  Logger() = default;
  virtual ~Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(Logger&&) = delete;

  virtual void Log(LogLevel level, std::string_view message) = 0;
};

// A logger that appends one line per message to `path`:
//
//   2026-09-24T10:15:30.123456Z INFO  [4711] recovered 3 log files
//
// The timestamp is UTC wall-clock time and the bracketed number is the kernel
// thread id. Messages below `min_level` are discarded. Each line is flushed to
// the operating system before Log returns, so a process crash keeps it; lines
// are not fsynced, because a log is diagnostic and must not slow the engine.
[[nodiscard]] Result<std::unique_ptr<Logger>> NewFileLogger(Env* env, const std::string& path,
                                                            LogLevel min_level = LogLevel::kInfo);

// Formats and logs a message; a null logger is allowed and does nothing, so
// call sites need no "is logging configured" check.
template <typename... Args>
void Log(Logger* logger, LogLevel level, std::format_string<Args...> format, Args&&... args) {
  if (logger != nullptr) {
    logger->Log(level, std::format(format, std::forward<Args>(args)...));
  }
}

}  // namespace strata
