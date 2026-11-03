#pragma once

#include "strata/logger.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace strata {

// A latency distribution in microseconds. Percentiles come from log-linear
// buckets, so each is within 6.25% of the true sample value (see Histogram).
struct LatencyStats {
  std::uint64_t count = 0;
  double mean = 0.0;
  double p50 = 0.0;
  double p90 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
  double max = 0.0;
};

// One level of the LSM tree.
struct LevelStats {
  int level = 0;
  std::uint64_t files = 0;
  std::uint64_t bytes = 0;
  // Bytes the level may hold before compaction moves data down. Level 0 is
  // limited by its table count instead, so its target is 0.
  std::uint64_t target_bytes = 0;
  // How urgently the level needs compacting; 1 or more means it does.
  double compaction_score = 0.0;
};

// The compaction running now, if any. Progress advances each time an output
// table is finished, so it moves in steps of about Options::max_file_size.
struct CompactionProgress {
  bool running = false;
  int level = 0;  // the input level; the output level is level + 1
  std::uint64_t input_bytes = 0;
  std::uint64_t bytes_written = 0;
};

// One field of an Event. Numbers are kept apart from text so JSON output can
// write them unquoted.
struct EventField {
  std::string key;
  std::string value;
  bool is_number = false;
};

// Something the engine did that an operator may want to see: opening,
// replaying the log, flushing, compacting, failing. Every event is also
// written to the info log as one key=value line.
struct Event {
  std::uint64_t sequence = 0;  // 1 for the first event since Open
  std::int64_t unix_micros = 0;
  LogLevel level = LogLevel::kInfo;
  std::string name;
  std::vector<EventField> fields;
};

// A point-in-time view of a database, from DB::GetStats. Taking one is cheap
// (no I/O) and safe at any rate from any thread. Metric names are stable;
// docs/observability.md lists every one.
struct Stats {
  std::int64_t unix_micros = 0;  // when this view was taken
  std::uint64_t uptime_micros = 0;

  // Monotonic totals since Open. A rate is the difference between two views
  // divided by the time between them.
  std::map<std::string, std::uint64_t> counters;
  // Current values that go up and down.
  std::map<std::string, std::uint64_t> gauges;
  // "get" and "write": time from the call to its return.
  std::map<std::string, LatencyStats> latencies;

  std::vector<LevelStats> levels;
  CompactionProgress compaction;

  // What Open did, in order: kept for the database's lifetime, so it is still
  // visible after many later events.
  std::vector<Event> recovery_events;
  // The most recent events, oldest first.
  std::vector<Event> recent_events;

  // Empty while healthy. Otherwise the error that made the database refuse
  // further writes.
  std::string background_error;
};

// `stats` as one JSON object; docs/observability.md gives the schema.
[[nodiscard]] std::string StatsToJson(const Stats& stats);

}  // namespace strata
