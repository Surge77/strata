#include "util/event_log.hpp"

#include <chrono>
#include <utility>

namespace strata {
namespace {

bool NeedsQuoting(std::string_view text) {
  if (text.empty()) {
    return true;
  }
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7F || c == '=' || c == '"' || c == '\\') {
      return true;
    }
  }
  return false;
}

void AppendQuoted(std::string* out, std::string_view text) {
  constexpr std::string_view kHex = "0123456789abcdef";
  out->push_back('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(c);
    } else if (c == '\n') {
      out->append("\\n");
    } else if (c == '\t') {
      out->append("\\t");
    } else if (byte < 0x20 || byte == 0x7F) {
      out->append("\\x");
      out->push_back(kHex[byte >> 4U]);
      out->push_back(kHex[byte & 0xFU]);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

void AppendValue(std::string* out, std::string_view value) {
  if (NeedsQuoting(value)) {
    AppendQuoted(out, value);
  } else {
    out->append(value);
  }
}

std::int64_t NowUnixMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::string FormatLogfmt(std::string_view name, const std::vector<EventField>& fields) {
  std::string line = "event=";
  AppendValue(&line, name);
  for (const EventField& field : fields) {
    line.push_back(' ');
    line.append(field.key);
    line.push_back('=');
    AppendValue(&line, field.value);
  }
  return line;
}

void EventLog::SetLogger(Logger* logger) {
  const std::lock_guard<std::mutex> guard(mutex_);
  logger_ = logger;
}

void EventLog::Record(LogLevel level, std::string name, std::vector<EventField> fields) {
  // Formatted outside the lock; the logger has its own.
  const std::string line = FormatLogfmt(name, fields);
  Event event{.sequence = 0,
              .unix_micros = NowUnixMicros(),
              .level = level,
              .name = std::move(name),
              .fields = std::move(fields)};
  Logger* logger = nullptr;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    event.sequence = next_sequence_++;
    logger = logger_;
    if (!opened_ && recovery_.size() < kMaxRecoveryEvents) {
      recovery_.push_back(event);
    }
    if (recent_capacity_ > 0) {
      if (recent_.size() == recent_capacity_) {
        recent_.pop_front();
      }
      recent_.push_back(std::move(event));
    }
  }
  if (logger != nullptr) {
    logger->Log(level, line);
  }
}

void EventLog::MarkOpened() {
  const std::lock_guard<std::mutex> guard(mutex_);
  opened_ = true;
}

std::vector<Event> EventLog::RecoveryEvents() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return recovery_;
}

std::vector<Event> EventLog::RecentEvents() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return {recent_.begin(), recent_.end()};
}

}  // namespace strata
