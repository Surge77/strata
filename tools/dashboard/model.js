// Pure functions over /stats snapshots: no DOM, no network, so node --test can
// check them. The dashboard reads nothing but the JSON that /stats returns.

// Counters are cumulative since Open; a rate needs two snapshots. A counter
// that went backwards means the database was reopened, so there is no rate.
export function ratePerSecond(previous, current, name) {
  if (!previous || !current) return null;
  const seconds = (current.unix_micros - previous.unix_micros) / 1e6;
  const before = previous.counters?.[name];
  const after = current.counters?.[name];
  if (!(seconds > 0) || before === undefined || after === undefined || after < before) {
    return null;
  }
  return (after - before) / seconds;
}

// Whether `current` continues the session `previous` came from. Uptime only
// grows while a database stays open.
export function sameSession(previous, current) {
  return Boolean(previous && current && current.uptime_micros >= previous.uptime_micros);
}

// Latencies are histograms since Open, so their percentiles barely move after
// an hour. Subtracting the previous snapshot's buckets gives the distribution
// of just the samples taken in between. Buckets are [upper_bound_micros, count]
// pairs, non-empty only. Returns null when the interval holds no samples.
export function intervalLatency(previousLatency, currentLatency, percentiles = [50, 99]) {
  if (!currentLatency?.buckets) return null;
  const before = new Map((previousLatency?.buckets ?? []).map(([bound, count]) => [bound, count]));
  const delta = [];
  let total = 0;
  for (const [bound, count] of currentLatency.buckets) {
    const added = count - (before.get(bound) ?? 0);
    if (added > 0) {
      delta.push([bound, added]);
      total += added;
    }
  }
  if (total === 0) return null;
  delta.sort((a, b) => a[0] - b[0]);
  const result = { count: total };
  for (const p of percentiles) {
    result[`p${p}`] = percentileOf(delta, total, p);
  }
  return result;
}

// The bucket bound at which `p` percent of `total` samples are at or below.
// Bounds are within 6.25% of the samples they hold (log-linear buckets).
export function percentileOf(sortedBuckets, total, p) {
  const threshold = (total * p) / 100;
  let seen = 0;
  for (const [bound, count] of sortedBuckets) {
    seen += count;
    if (seen >= threshold) return bound;
  }
  return sortedBuckets.length ? sortedBuckets[sortedBuckets.length - 1][0] : 0;
}

export function writeAmplification(counters) {
  const written = counters?.["write.bytes"] ?? 0;
  if (written === 0) return null;
  return ((counters["flush.bytes"] ?? 0) + (counters["compaction.bytes_written"] ?? 0)) / written;
}

export function hitRatio(hits, misses) {
  const total = (hits ?? 0) + (misses ?? 0);
  return total === 0 ? null : hits / total;
}

export function compactionProgress(compaction) {
  if (!compaction?.running || !(compaction.input_bytes > 0)) return null;
  return Math.min(1, compaction.bytes_written / compaction.input_bytes);
}

// Health as the dashboard shows it: the engine's own verdict first, then
// whether the data is fresh.
export function health(stats, secondsSinceLastSnapshot, staleAfterSeconds = 5) {
  if (!stats) return { state: "connecting", text: "connecting" };
  if (secondsSinceLastSnapshot > staleAfterSeconds) {
    return { state: "stale", text: "no response" };
  }
  if (stats.background_error) return { state: "error", text: "refusing writes" };
  return { state: "ok", text: "healthy" };
}

// A fixed-size history of [seconds, value] points for one chart line.
export class Series {
  constructor(capacity) {
    this.capacity = capacity;
    this.points = [];
  }
  push(seconds, value) {
    this.points.push([seconds, value]);
    if (this.points.length > this.capacity) this.points.shift();
  }
  clear() {
    this.points = [];
  }
  last() {
    for (let i = this.points.length - 1; i >= 0; i -= 1) {
      if (this.points[i][1] !== null) return this.points[i][1];
    }
    return null;
  }
}

const BYTE_UNITS = ["B", "KiB", "MiB", "GiB", "TiB"];

export function formatBytes(bytes) {
  if (bytes === null || bytes === undefined) return "–";
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < BYTE_UNITS.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(unit === 0 || value >= 100 ? 0 : 1)} ${BYTE_UNITS[unit]}`;
}

export function formatCount(value) {
  if (value === null || value === undefined) return "–";
  if (value >= 1e9) return `${(value / 1e9).toFixed(2)}G`;
  if (value >= 1e6) return `${(value / 1e6).toFixed(2)}M`;
  if (value >= 1e4) return `${(value / 1e3).toFixed(1)}k`;
  return value >= 100 ? value.toFixed(0) : value.toFixed(value % 1 === 0 ? 0 : 1);
}

export function formatMicros(micros) {
  if (micros === null || micros === undefined) return "–";
  if (micros === 0) return "0";
  if (micros >= 1e6) return `${(micros / 1e6).toFixed(2)} s`;
  if (micros >= 1e3) return `${(micros / 1e3).toFixed(micros >= 1e5 ? 0 : 1)} ms`;
  return `${micros.toFixed(micros >= 100 ? 0 : micros >= 10 ? 1 : 2)} µs`;
}

export function formatDuration(micros) {
  let seconds = Math.floor(micros / 1e6);
  const days = Math.floor(seconds / 86400);
  seconds -= days * 86400;
  const hours = Math.floor(seconds / 3600);
  seconds -= hours * 3600;
  const minutes = Math.floor(seconds / 60);
  seconds -= minutes * 60;
  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m ${seconds}s`;
  return `${seconds}s`;
}

export function formatPercent(ratio) {
  return ratio === null || ratio === undefined ? "–" : `${(ratio * 100).toFixed(1)}%`;
}

// "flush_finished table=12 bytes=40960" style text for one event's fields.
export function eventFieldsText(event) {
  return Object.entries(event.fields ?? {})
    .map(([key, value]) => `${key}=${value}`)
    .join("  ");
}

export function formatClock(unixMicros) {
  const date = new Date(unixMicros / 1000);
  const pad = (n, width = 2) => String(n).padStart(width, "0");
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(
    date.getMilliseconds(),
    3,
  )}`;
}
