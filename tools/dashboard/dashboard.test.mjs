// node --test tools/dashboard/dashboard.test.mjs
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import { niceMax } from "./charts.js";
import {
  Series, compactionProgress, formatBytes, formatDuration, formatMicros, health, hitRatio,
  intervalLatency, percentileOf, ratePerSecond, sameSession, writeAmplification,
} from "./model.js";

const here = dirname(fileURLToPath(import.meta.url));

const snapshot = (micros, counters, extra = {}) => ({
  unix_micros: micros, uptime_micros: micros, counters, ...extra,
});

test("a rate is the counter's growth divided by the time between snapshots", () => {
  const a = snapshot(1_000_000, { "get.calls": 100 });
  const b = snapshot(3_000_000, { "get.calls": 700 });
  assert.equal(ratePerSecond(a, b, "get.calls"), 300);
});

test("no rate without two snapshots, a time step, or a counter that only grew", () => {
  const a = snapshot(1_000_000, { "get.calls": 100 });
  assert.equal(ratePerSecond(null, a, "get.calls"), null);
  assert.equal(ratePerSecond(a, a, "get.calls"), null);
  assert.equal(ratePerSecond(a, snapshot(2_000_000, { "get.calls": 5 }), "get.calls"), null);
  assert.equal(ratePerSecond(a, snapshot(2_000_000, {}), "get.calls"), null);
});

test("a shrinking uptime marks a reopened database", () => {
  const a = { uptime_micros: 9_000_000 };
  assert.equal(sameSession(a, { uptime_micros: 10_000_000 }), true);
  assert.equal(sameSession(a, { uptime_micros: 1_000 }), false);
  assert.equal(sameSession(null, a), false);
});

test("interval latency uses only the samples taken between two snapshots", () => {
  // Since Open: 90 fast samples. In the interval: 10 more fast and 10 slow.
  const before = { buckets: [[1, 90]] };
  const after = { buckets: [[1, 100], [500, 10]] };
  const interval = intervalLatency(before, after);
  assert.equal(interval.count, 20);
  assert.equal(interval.p50, 1);
  assert.equal(interval.p99, 500);
  // The cumulative distribution would have hidden the slow half: p50 = 1, p90 = 1.
});

test("interval latency is null when nothing was sampled, and whole without a baseline", () => {
  const buckets = { buckets: [[2, 4]] };
  assert.equal(intervalLatency(buckets, buckets), null);
  assert.equal(intervalLatency(undefined, buckets).count, 4);
  assert.equal(intervalLatency(buckets, {}), null);
});

test("a percentile is the first bucket bound reaching its share of samples", () => {
  const buckets = [[1, 50], [2, 40], [8, 9], [100, 1]];
  assert.equal(percentileOf(buckets, 100, 50), 1);
  assert.equal(percentileOf(buckets, 100, 90), 2);
  assert.equal(percentileOf(buckets, 100, 99), 8);
  assert.equal(percentileOf(buckets, 100, 100), 100);
});

test("write amplification is bytes written by flushes and compactions per user byte", () => {
  assert.equal(writeAmplification({ "write.bytes": 100, "flush.bytes": 110, "compaction.bytes_written": 290 }), 4);
  assert.equal(writeAmplification({ "write.bytes": 0 }), null);
  assert.equal(hitRatio(3, 1), 0.75);
  assert.equal(hitRatio(0, 0), null);
});

test("compaction progress is bytes written over input, capped at 1", () => {
  assert.equal(compactionProgress({ running: true, input_bytes: 200, bytes_written: 50 }), 0.25);
  assert.equal(compactionProgress({ running: true, input_bytes: 100, bytes_written: 150 }), 1);
  assert.equal(compactionProgress({ running: false, input_bytes: 100, bytes_written: 0 }), null);
});

test("health puts a stopped engine before freshness, and staleness before health", () => {
  assert.equal(health(null, 0).state, "connecting");
  assert.equal(health({ background_error: "" }, 1).state, "ok");
  assert.equal(health({ background_error: "IO error: disk full" }, 1).state, "error");
  assert.equal(health({ background_error: "" }, 30).state, "stale");
});

test("a series keeps only its newest points and remembers the last value", () => {
  const series = new Series(3);
  [1, 2, 3, 4].forEach((v) => series.push(v, v * 10));
  assert.deepEqual(series.points.map(([t]) => t), [2, 3, 4]);
  series.push(5, null);
  assert.equal(series.last(), 40);
});

test("formatting", () => {
  assert.equal(formatBytes(512), "512 B");
  assert.equal(formatBytes(1536), "1.5 KiB");
  assert.equal(formatBytes(200 * 1024 * 1024), "200 MiB");
  assert.equal(formatMicros(0.42), "0.42 µs");
  assert.equal(formatMicros(1500), "1.5 ms");
  assert.equal(formatDuration(3_725_000_000), "1h 2m");
  assert.equal(niceMax(0), 1);
  assert.equal(niceMax(730), 1000);
  assert.equal(niceMax(2100), 2500);
});

// Phase 13's merge gate: the dashboard consumes only the JSON API. Every
// script is checked, so a new fetch, an absolute URL, or a module from
// elsewhere fails here.
test("the dashboard requests nothing but /stats", () => {
  const scripts = readdirSync(here).filter((name) => name.endsWith(".js"));
  assert.ok(scripts.includes("dashboard.js"));
  for (const name of scripts) {
    const source = readFileSync(join(here, name), "utf8");
    assert.doesNotMatch(source, /https?:\/\//, `${name} names an absolute URL`);
    assert.doesNotMatch(source, /XMLHttpRequest|WebSocket|EventSource|sendBeacon|importScripts/,
      `${name} uses another way to reach the network`);
    for (const [, target] of source.matchAll(/fetch\(\s*([^,)]+)/g)) {
      assert.equal(target.trim(), "STATS_URL", `${name} fetches ${target}`);
    }
    for (const [, specifier] of source.matchAll(/from\s+"([^"]+)"/g)) {
      assert.match(specifier, /^\.\/[a-z]+\.js$/, `${name} imports ${specifier}`);
    }
  }
  const dashboard = readFileSync(join(here, "dashboard.js"), "utf8");
  assert.match(dashboard, /const STATS_URL = "\/stats";/);
  const html = readFileSync(join(here, "index.html"), "utf8");
  for (const [, url] of html.matchAll(/(?:src|href)="([^"]+)"/g)) {
    assert.match(url, /^\/[a-z]+\.(js|css)$/, `index.html loads ${url}`);
  }
});
