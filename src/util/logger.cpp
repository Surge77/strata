#include "strata/logger.hpp"

#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <unistd.h>

namespace strata {
namespace {

class FileLogger final : public Logger {
 public:
  FileLogger(std::unique_ptr<WritableFile> file, LogLevel min_level)
      : file_(std::move(file)), min_level_(min_level) {}

  ~FileLogger() override { static_cast<void>(file_->Close()); }
  FileLogger(const FileLogger&) = delete;
  FileLogger& operator=(const FileLogger&) = delete;
  FileLogger(FileLogger&&) = delete;
  FileLogger& operator=(FileLogger&&) = delete;

  void Log(LogLevel level, std::string_view message) override {
    if (level < min_level_) {
      return;
    }
    // The line is built before taking the lock, so the critical section is
    // only the write itself.
    const auto now =
        std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
    const std::string line = std::format("{:%FT%T}Z {:<5} [{}] {}\n", now, LogLevelName(level),
                                         static_cast<long>(::gettid()), message);

    // One Append plus Flush per line under the mutex is what keeps lines from
    // concurrent threads whole. A failed write is dropped: logging must never
    // turn into an engine error.
    const std::lock_guard<std::mutex> guard(mutex_);
    if (file_->Append(line).ok()) {
      static_cast<void>(file_->Flush());
    }
  }

 private:
  std::mutex mutex_;
  std::unique_ptr<WritableFile> file_;
  LogLevel min_level_;
};

}  // namespace

std::string_view LogLevelName(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
  }
  return "?";
}

Result<std::unique_ptr<Logger>> NewFileLogger(Env* env, const std::string& path,
                                              LogLevel min_level) {
  STRATA_ASSIGN_OR_RETURN(auto file, env->NewAppendableFile(path));
  return std::unique_ptr<Logger>(std::make_unique<FileLogger>(std::move(file), min_level));
}

}  // namespace strata
