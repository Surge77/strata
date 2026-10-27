#pragma once

#include "strata/logger.hpp"
#include "strata/stats.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

[[nodiscard]] inline EventField Field(std::string key, std::string value) {
  return EventField{.key = std::move(key), .value = std::move(value), .is_number = false};
}

template <std::integral T>
[[nodiscard]] EventField Field(std::string key, T value) {
  return EventField{.key = std::move(key), .value = std::to_string(value), .is_number = true};
}

// `name` and `fields` as one logfmt line: event=flush table=12 file="a b".
// Text is quoted when it is empty or holds a space, '=', '"', or a control
// character, with '"' and '\' escaped and control characters as \n, \t, or
// \xNN, so one event is always exactly one line.
[[nodiscard]] std::string FormatLogfmt(std::string_view name,
                                       const std::vector<EventField>& fields);

// The engine's structured events: each is written to the info log as a
// logfmt line and kept in memory for the stats API.
//
// Two windows are kept. Events recorded before MarkOpened() -- what Open did
// to recover the database -- are kept for the database's lifetime, up to
// kMaxRecoveryEvents. Every event also enters a ring of the last
// `recent_capacity`, where routine flushes and compactions soon push old ones
// out. Thread-safe.
class EventLog {
 public:
  static constexpr std::size_t kMaxRecoveryEvents = 256;

  EventLog(Logger* logger, std::size_t recent_capacity)
      : logger_(logger), recent_capacity_(recent_capacity) {}

  // `logger` may change once Open has created the info log.
  void SetLogger(Logger* logger);

  void Record(LogLevel level, std::string name, std::vector<EventField> fields = {});
  void MarkOpened();

  [[nodiscard]] std::vector<Event> RecoveryEvents() const;
  [[nodiscard]] std::vector<Event> RecentEvents() const;

 private:
  mutable std::mutex mutex_;
  Logger* logger_;
  const std::size_t recent_capacity_;
  std::uint64_t next_sequence_ = 1;
  bool opened_ = false;
  std::vector<Event> recovery_;
  std::deque<Event> recent_;
};

}  // namespace strata
