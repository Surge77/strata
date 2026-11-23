// StatsToJson: the wire format of the stats endpoint. The schema is
// documented in docs/observability.md; keep the two in step.

#include "strata/stats.hpp"

#include <string>
#include <vector>

#include "util/json.hpp"

namespace strata {
namespace {

// Appends `"key":` with a leading comma for every member but the first.
void Key(std::string* out, std::string_view key, bool* first) {
  if (!*first) {
    out->push_back(',');
  }
  *first = false;
  AppendJsonString(out, key);
  out->push_back(':');
}

template <typename Number>
void Member(std::string* out, std::string_view key, Number value, bool* first) {
  Key(out, key, first);
  AppendJsonNumber(out, value);
}

void AppendCounterMap(std::string* out, const std::map<std::string, std::uint64_t>& values) {
  out->push_back('{');
  bool first = true;
  for (const auto& [name, value] : values) {
    Member(out, name, value, &first);
  }
  out->push_back('}');
}

void AppendLatency(std::string* out, const LatencyStats& latency) {
  out->push_back('{');
  bool first = true;
  Member(out, "count", latency.count, &first);
  Member(out, "mean", latency.mean, &first);
  Member(out, "p50", latency.p50, &first);
  Member(out, "p90", latency.p90, &first);
  Member(out, "p99", latency.p99, &first);
  Member(out, "p999", latency.p999, &first);
  Member(out, "max", latency.max, &first);
  out->push_back('}');
}

void AppendLevel(std::string* out, const LevelStats& level) {
  out->push_back('{');
  bool first = true;
  Member(out, "level", static_cast<std::int64_t>(level.level), &first);
  Member(out, "files", level.files, &first);
  Member(out, "bytes", level.bytes, &first);
  Member(out, "target_bytes", level.target_bytes, &first);
  Member(out, "compaction_score", level.compaction_score, &first);
  out->push_back('}');
}

void AppendCompaction(std::string* out, const CompactionProgress& compaction) {
  out->push_back('{');
  bool first = true;
  Key(out, "running", &first);
  out->append(compaction.running ? "true" : "false");
  Member(out, "level", static_cast<std::int64_t>(compaction.level), &first);
  Member(out, "input_bytes", compaction.input_bytes, &first);
  Member(out, "bytes_written", compaction.bytes_written, &first);
  out->push_back('}');
}

void AppendEvent(std::string* out, const Event& event) {
  out->push_back('{');
  bool first = true;
  Member(out, "sequence", event.sequence, &first);
  Member(out, "unix_micros", event.unix_micros, &first);
  Key(out, "level", &first);
  AppendJsonString(out, LogLevelName(event.level));
  Key(out, "name", &first);
  AppendJsonString(out, event.name);
  Key(out, "fields", &first);
  out->push_back('{');
  bool first_field = true;
  for (const EventField& field : event.fields) {
    Key(out, field.key, &first_field);
    if (field.is_number) {
      out->append(field.value);  // produced by std::to_string of an integer
    } else {
      AppendJsonString(out, field.value);
    }
  }
  out->append("}}");
}

template <typename T, typename AppendOne>
void AppendArray(std::string* out, const std::vector<T>& items, AppendOne append_one) {
  out->push_back('[');
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out->push_back(',');
    }
    append_one(out, items[i]);
  }
  out->push_back(']');
}

}  // namespace

std::string StatsToJson(const Stats& stats) {
  std::string out = "{";
  bool first = true;
  Member(&out, "unix_micros", stats.unix_micros, &first);
  Member(&out, "uptime_micros", stats.uptime_micros, &first);
  Key(&out, "counters", &first);
  AppendCounterMap(&out, stats.counters);
  Key(&out, "gauges", &first);
  AppendCounterMap(&out, stats.gauges);

  Key(&out, "latencies", &first);
  out.push_back('{');
  bool first_latency = true;
  for (const auto& [name, latency] : stats.latencies) {
    Key(&out, name, &first_latency);
    AppendLatency(&out, latency);
  }
  out.push_back('}');

  Key(&out, "levels", &first);
  AppendArray(&out, stats.levels, AppendLevel);
  Key(&out, "compaction", &first);
  AppendCompaction(&out, stats.compaction);
  Key(&out, "recovery_events", &first);
  AppendArray(&out, stats.recovery_events, AppendEvent);
  Key(&out, "recent_events", &first);
  AppendArray(&out, stats.recent_events, AppendEvent);
  Key(&out, "background_error", &first);
  if (stats.background_error.empty()) {
    out.append("null");
  } else {
    AppendJsonString(&out, stats.background_error);
  }
  out.push_back('}');
  return out;
}

}  // namespace strata
