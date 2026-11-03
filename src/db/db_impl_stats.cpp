// DBImpl's stats view: the metrics it registers and CollectStats, which
// assembles the public Stats from them (docs/observability.md).

#include <chrono>

#include "db/compaction.hpp"
#include "db/db_impl.hpp"

namespace strata {
namespace {

std::int64_t NowUnixMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

HotMetrics::HotMetrics(MetricsRegistry* registry)
    : get_found(registry->AddCounter("get.found")),
      get_memtable_answers(registry->AddCounter("get.memtable_answers")),
      get_table_reads(registry->AddCounter("get.table_reads")),
      get_filter_skips(registry->AddCounter("get.filter_skips")),
      iterators(registry->AddCounter("iterator.created")),
      get_nanos(registry->AddLatency("get")),
      write_nanos(registry->AddLatency("write")) {}

Result<Stats> DBImpl::CollectStats() {
  Stats stats;
  stats.unix_micros = NowUnixMicros();
  stats.uptime_micros =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - opened_at_)
                                     .count());

  DBStats engine;
  std::shared_ptr<const Version> version;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) {
      return Status::InvalidArgument("database is closed");
    }
    engine = stats_;
    version = versions_->current();
    stats.gauges["memtable.bytes"] = mem_->ApproximateMemoryUsage();
    stats.gauges["immutable_memtable.bytes"] = imm_ != nullptr ? imm_->ApproximateMemoryUsage() : 0;
    stats.gauges["sequence.last"] = last_sequence_;
    stats.gauges["snapshots.live"] = snapshots_.size();
    stats.gauges["writers.queued"] = writers_.empty() ? 0 : writers_.size() - 1;
    stats.compaction = CompactionProgress{
        .running = compaction_running_,
        .level = compaction_running_ ? compaction_level_ : 0,
        .input_bytes = compaction_running_ ? compaction_input_bytes_ : 0,
        .bytes_written =
            compaction_running_ ? compaction_bytes_written_.load(std::memory_order_relaxed) : 0,
    };
    if (!background_error_.ok()) {
      stats.background_error = background_error_.ToString();
    }
  }

  // Everything below reads state with its own synchronization.
  stats.counters = metrics_.CounterValues();
  stats.latencies = metrics_.LatencyValues();
  auto& counters = stats.counters;
  counters["get.calls"] = stats.latencies.at("get").count;
  counters["write.calls"] = stats.latencies.at("write").count;
  counters["write.groups"] = engine.write_groups;
  counters["write.bytes"] = engine.user_bytes_written;
  counters["write.syncs"] = engine.log_syncs;
  counters["write.slowdowns"] = engine.write_slowdowns;
  counters["write.stops"] = engine.write_stops;
  counters["log.rotations"] = engine.log_rotations;
  counters["flush.count"] = engine.flushes;
  counters["flush.bytes"] = engine.flush_bytes_written;
  counters["compaction.count"] = engine.compactions;
  counters["compaction.trivial_moves"] = engine.trivial_moves;
  counters["compaction.bytes_read"] = engine.compaction_bytes_read;
  counters["compaction.bytes_written"] = engine.compaction_bytes_written;

  const LruCacheStats blocks = table_cache_->BlockStats();
  const LruCacheStats tables = table_cache_->TableStats();
  counters["block_cache.hits"] = blocks.hits;
  counters["block_cache.misses"] = blocks.misses;
  counters["table_cache.hits"] = tables.hits;
  counters["table_cache.misses"] = tables.misses;
  stats.gauges["block_cache.bytes"] = blocks.usage;
  stats.gauges["block_cache.capacity_bytes"] = blocks.capacity;
  stats.gauges["table_cache.tables"] = tables.entries;
  stats.gauges["table_cache.capacity"] = tables.capacity;

  const std::uint64_t base = options_.max_bytes_for_level_base;
  for (int level = 0; level < kNumLevels; ++level) {
    stats.levels.push_back(LevelStats{
        .level = level,
        .files = version->files(level).size(),
        .bytes = version->LevelBytes(level),
        .target_bytes = level == 0 ? 0 : MaxBytesForLevel(level, base),
        // The last level is never compacted further, so it has no score.
        .compaction_score = level + 1 < kNumLevels ? CompactionScore(*version, level, base) : 0.0,
    });
  }

  stats.recovery_events = events_.RecoveryEvents();
  stats.recent_events = events_.RecentEvents();
  return stats;
}

}  // namespace strata
