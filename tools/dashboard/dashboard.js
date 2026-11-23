// Polls /stats once a second and renders it. /stats is the only thing this
// page requests besides its own files (a test enforces that), so anything it
// shows, any other client of the JSON API could show too.
import { drawChart } from "./charts.js";
import {
  Series, compactionProgress, eventFieldsText, formatBytes, formatClock, formatCount,
  formatDuration, formatMicros, formatPercent, health, hitRatio, intervalLatency,
  ratePerSecond, sameSession, writeAmplification,
} from "./model.js";

const STATS_URL = "/stats";
const POLL_MS = 1000;
const REQUEST_TIMEOUT_MS = 2000;
const WINDOW_SECONDS = 300;
const HISTORY = WINDOW_SECONDS + 5;
const MIB = 1024 * 1024;

const $ = (id) => document.getElementById(id);
const color = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

const series = {
  getRate: new Series(HISTORY), writeRate: new Series(HISTORY),
  getP50: new Series(HISTORY), getP99: new Series(HISTORY),
  writeP50: new Series(HISTORY), writeP99: new Series(HISTORY),
  flushRate: new Series(HISTORY), compactionRate: new Series(HISTORY),
};

const charts = [
  {
    canvas: "chart-throughput", legend: "legend-throughput", format: formatCount,
    lines: [
      { label: "gets/s", series: series.getRate, colorVar: "--line-get" },
      { label: "writes/s", series: series.writeRate, colorVar: "--line-write" },
    ],
  },
  {
    canvas: "chart-latency", legend: "legend-latency", format: formatMicros,
    lines: [
      { label: "get p50", series: series.getP50, colorVar: "--line-get" },
      { label: "get p99", series: series.getP99, colorVar: "--line-get-tail" },
      { label: "write p50", series: series.writeP50, colorVar: "--line-write" },
      { label: "write p99", series: series.writeP99, colorVar: "--line-write-tail" },
    ],
  },
  {
    canvas: "chart-io", legend: "legend-io", format: (v) => `${formatBytes(v)}/s`,
    lines: [
      { label: "flush", series: series.flushRate, colorVar: "--line-flush" },
      { label: "compaction", series: series.compactionRate, colorVar: "--line-compaction" },
    ],
  },
];

let previous = null;
let latest = null;
let lastSuccessMs = 0;
let lastError = "";

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setText(id, text) {
  const node = $(id);
  if (node.textContent !== text) node.textContent = text;
}

function record(stats) {
  if (!sameSession(previous, stats)) {
    Object.values(series).forEach((s) => s.clear());
    previous = null;
  }
  const t = stats.unix_micros / 1e6;
  const getLatency = intervalLatency(previous?.latencies?.get, stats.latencies?.get);
  const writeLatency = intervalLatency(previous?.latencies?.write, stats.latencies?.write);
  series.getRate.push(t, ratePerSecond(previous, stats, "get.calls"));
  series.writeRate.push(t, ratePerSecond(previous, stats, "write.calls"));
  series.getP50.push(t, getLatency?.p50 ?? null);
  series.getP99.push(t, getLatency?.p99 ?? null);
  series.writeP50.push(t, writeLatency?.p50 ?? null);
  series.writeP99.push(t, writeLatency?.p99 ?? null);
  series.flushRate.push(t, ratePerSecond(previous, stats, "flush.bytes"));
  series.compactionRate.push(t, ratePerSecond(previous, stats, "compaction.bytes_written"));
  previous = stats;
  latest = stats;
}

function renderStats(stats) {
  const counters = stats.counters ?? {};
  setText("stat-get-rate", formatCount(series.getRate.last()));
  setText("stat-write-rate", formatCount(series.writeRate.last()));
  setText("stat-get-p99", formatMicros(series.getP99.last()));
  setText("stat-write-p99", formatMicros(series.writeP99.last()));
  const amplification = writeAmplification(counters);
  setText("stat-write-amp", amplification === null ? "–" : `${amplification.toFixed(2)}×`);
  setText("stat-cache-hits",
    formatPercent(hitRatio(counters["block_cache.hits"], counters["block_cache.misses"])));
  setText("uptime", formatDuration(stats.uptime_micros));
  setText("sequence", formatCount(stats.gauges?.["sequence.last"] ?? null));
  setText("snapshots", String(stats.gauges?.["snapshots.live"] ?? "–"));
  setText("updated", formatClock(stats.unix_micros));
}

function scoreLevel(score) {
  if (score >= 2) return "bad";
  if (score >= 1) return "warn";
  return "ok";
}

function renderLevels(levels) {
  const rows = (levels ?? []).map((level) => {
    const row = element("tr");
    const fill = level.level === 0 ? level.compaction_score : level.bytes / (level.target_bytes || 1);
    const bar = element("div", "bar");
    const barFill = element("div", "bar-fill");
    barFill.style.width = `${Math.min(100, Math.max(0, fill * 100)).toFixed(1)}%`;
    barFill.dataset.level = scoreLevel(level.compaction_score);
    bar.append(barFill);
    const barCell = element("td");
    barCell.append(bar);
    const score = element("td", "score", level.compaction_score.toFixed(2));
    score.dataset.level = scoreLevel(level.compaction_score);
    row.append(element("td", "", `L${level.level}`), element("td", "", String(level.files)),
      element("td", "", formatBytes(level.bytes)), barCell, score);
    return row;
  });
  $("levels").replaceChildren(...rows);
}

function definitionList(id, entries) {
  const nodes = entries.flatMap(([term, value]) => [element("dt", "", term), element("dd", "", value)]);
  $(id).replaceChildren(...nodes);
}

function renderCompaction(stats) {
  const compaction = stats.compaction ?? {};
  const progress = compactionProgress(compaction);
  setText("compaction-state", compaction.running
    ? `L${compaction.level} → L${compaction.level + 1}  ${formatPercent(progress)}` : "idle");
  $("compaction-fill").style.width = `${((progress ?? 0) * 100).toFixed(1)}%`;
  definitionList("compaction-detail", compaction.running
    ? [["input", formatBytes(compaction.input_bytes)], ["written", formatBytes(compaction.bytes_written)]]
    : [["compactions", formatCount(stats.counters?.["compaction.count"] ?? 0)],
       ["trivial moves", formatCount(stats.counters?.["compaction.trivial_moves"] ?? 0)]]);
  const g = stats.gauges ?? {};
  definitionList("gauges", [
    ["memtable", formatBytes(g["memtable.bytes"])],
    ["immutable memtable", formatBytes(g["immutable_memtable.bytes"])],
    ["writers queued", String(g["writers.queued"] ?? "–")],
    ["block cache", `${formatBytes(g["block_cache.bytes"])} / ${formatBytes(g["block_cache.capacity_bytes"])}`],
    ["open tables", `${g["table_cache.tables"] ?? "–"} / ${g["table_cache.capacity"] ?? "–"}`],
  ]);
}

function renderEvents(id, events, newestFirst) {
  const list = [...(events ?? [])];
  if (newestFirst) list.reverse();
  const items = list.map((event) => {
    const item = element("li");
    const level = element("span", "level", event.level);
    level.dataset.level = event.level;
    item.append(element("span", "time", formatClock(event.unix_micros)), level,
      element("span", "name", event.name), element("span", "fields", eventFieldsText(event)));
    return item;
  });
  if (items.length === 0) items.push(element("li", "empty", "none"));
  $(id).replaceChildren(...items);
}

function renderCharts() {
  const now = (latest?.unix_micros ?? Date.now() * 1000) / 1e6;
  for (const chart of charts) {
    const lines = chart.lines.map((line) => ({ color: color(line.colorVar), points: line.series.points }));
    drawChart($(chart.canvas), { lines, now, windowSeconds: WINDOW_SECONDS, format: chart.format });
    const legend = chart.lines.map((line) => {
      const item = element("li");
      const swatch = element("span", "swatch");
      swatch.style.background = color(line.colorVar);
      item.append(swatch, element("span", "", line.label),
        element("span", "value", chart.format(line.series.last())));
      return item;
    });
    $(chart.legend).replaceChildren(...legend);
  }
}

function renderHealth() {
  const secondsSince = lastSuccessMs ? (Date.now() - lastSuccessMs) / 1000 : Infinity;
  const state = health(latest, latest ? secondsSince : 0);
  const status = $("status");
  status.dataset.state = state.state;
  setText("status", state.text);
  const message = latest?.background_error || (state.state === "stale" ? lastError : "");
  $("error").hidden = !message;
  setText("error", message);
}

function render() {
  if (latest) {
    renderStats(latest);
    renderLevels(latest.levels);
    renderCompaction(latest);
    renderEvents("recovery-events", latest.recovery_events, false);
    renderEvents("recent-events", latest.recent_events, true);
  }
  renderCharts();
  renderHealth();
}

async function poll() {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    const response = await fetch(STATS_URL, { cache: "no-store", signal: controller.signal });
    if (!response.ok) {
      throw new Error(`${STATS_URL}: ${response.status} ${(await response.text()).trim()}`);
    }
    record(await response.json());
    lastSuccessMs = Date.now();
    lastError = "";
  } catch (error) {
    lastError = error instanceof Error ? error.message : String(error);
  } finally {
    clearTimeout(timer);
  }
  render();
  setTimeout(poll, POLL_MS);
}

window.addEventListener("resize", renderCharts);
poll();
