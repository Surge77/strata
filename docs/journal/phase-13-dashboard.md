# Phase 13: Dashboard

**Branch:** `feature/phase-13-dashboard`.

## Goal

From the roadmap: LSM level view, compaction progress, latency charts, recovery events.
Merge gate: **the dashboard consumes only the JSON API.**

## Roles and methods

- **Principal engineer:** what does "consumes only the JSON API" have to mean to be worth a
  gate? Not "it happens to call /stats", but "a test fails the day it calls anything
  else". That became the boundary test below.
- **Product designer:** which operational dashboards do engineers already read? The
  reference was a Grafana-style operations board: dense dark panels, stat tiles over time
  series, conventional colours (amber at a threshold, red past it), no decoration. The
  user's standing rule for this workspace is to anchor UIs in real products rather than an
  AI house style.
- **Security reviewer (STRIDE, narrowed to Tampering and Information disclosure):** the
  stats carry text an attacker can choose (a table file name, an error string). What stops
  that text from running in the page, and what stops another web page from reading the
  dashboard?
- **Performance engineer:** latency percentiles since Open hardly move after an hour.
  What does a chart need from the API to show the last second instead?
- **Test engineer:** for each claim (only `/stats`, files embedded match their sources,
  routing, CSP), which test fails if it stops being true? Each was checked by breaking it.

## Design decisions

| Decision | Alternatives | Why |
|---|---|---|
| The engine's `StatsServer` serves the page (`StatsServerOptions::files`) | A separate web server; a file opened from disk | The endpoint deliberately sends no CORS headers, so a page from any other origin cannot read `/stats`. Same origin is the only way that keeps that defence |
| Files embedded into a `strata_dashboard` library at configure time | Read from disk at run time; ship them in the engine | A binary serves the dashboard with nothing on disk, and the engine library stays free of UI. Any program that embeds strata can pass `dashboard::Files()` |
| A CSP on every static file: `default-src 'none'`, scripts and styles only from `'self'`, `connect-src 'self'`, no framing, no referrer | No CSP; `unsafe-inline` | Stats text reaches the page. With no inline script allowed and everything rendered through `textContent`, there are two independent barriers between that text and code |
| Histogram buckets in the JSON (`[upper_micros, count]`, non-empty only) | Per-interval percentiles computed by the engine; cumulative percentiles only | The engine stays stateless about its readers, and any client can compute the last interval's percentiles by subtracting an earlier snapshot. Pairs rather than objects: a busy histogram has 100+ buckets, sent once a second |
| Plain ES modules, a canvas chart written for the purpose, no framework or chart library | React, Chart.js | About 600 lines in all. A dependency here would be the only third-party code in the repository's runtime, and it would need its own audit and pinning |
| Separate get and write latency charts | One latency chart | Synced writes are milliseconds and memtable gets are microseconds; on one axis the gets were a flat line at zero (found in the browser) |
| `stratasoak`, a mixed workload on one open database, can serve the dashboard | Add a server to `stratabench` | `stratabench` drives `DBImpl` directly and reopens the database every round. A long-running workload is also what phase 15's soak test needs |
| Routing moved to `obs/routes` | Keep it in `stats_server.cpp` | That file was at 303 lines, over the 300-line limit, and routing can now be unit-tested without sockets |

## Steps

1. `feat(obs)`: latency buckets in `Stats` and the JSON.
2. `feat(obs)`: static files beside `/stats`, under the CSP; routing split out and
   unit-tested; `Start` refuses files that would shadow an endpoint or inject a header.
3. `feat(dashboard)`: the page, its model, charts, the embedding, `stratactl serve`
   serving it, and the tests.
4. `feat(stratasoak)`: the workload, with `--serve_port`.
5. `fix(dashboard)`: the first build's failures (below).
6. `fix(dashboard)`: what the browser showed (below).
7. `docs(obs)`: the dashboard, its panels and sources, and the static-file policy.

## Verification (final tree, `f88d55d`)

- **Suites:** plain, ASan/UBSan, and TSan each pass **530/530** with GCC 13, and so do
  ASan/UBSan (with the libFuzzer targets) and TSan with clang 18. That is phase 12's 514
  plus the routing, embedded-file, bucket, and `stratasoak` CLI tests.
- **The gate, enforced:** `dashboard_js` (12 tests under Node 24) fails when any script
  fetches anything but `/stats`; adding `fetch("/admin")` to a module made it fail.
  `dashboard_files_test` fails when an embedded copy differs from its source.
- **The gate, observed:** in Chromium against a live `stratasoak` run, the page's resource
  list was its own five files, `/favicon.svg`, and `/stats`, and nothing else.
- **Coverage:** **96.9% of lines** (4678 of 4830).
- **Format:** clean.
- **Sync negative control:** **11 of 11** sites caught.
- **Note:** the full gate run was stopped once more by Windows memory pressure, during the
  negative control; that step was rerun on its own on the same commit.

## Review

An independent review found **no CRITICAL or HIGH** issues. It traced the security model
end to end: the Host check runs before any route, so the static files get the DNS-rebinding
defence too; files are an exact-match, in-memory list, so nothing on disk can be reached;
a content type cannot inject a header; and the CSP plus `textContent` keep stats text
inert. Findings, all MEDIUM and LOW fixed except one, deferred with its reason:

- **MEDIUM, fixed: one render error stopped polling for good.** The next poll was
  scheduled after `render()`, and a non-finite compaction score arrives as JSON `null`,
  which made `toFixed` throw. The poll is now scheduled in `finally`, a render error goes
  to the banner, and a missing score is drawn as 0.
- **MEDIUM, mitigated: a browser's speculative idle socket can hold the server for its
  2-second deadline**, the same as the page's fetch timeout. The fetch now waits 4.5
  seconds. The one-connection-at-a-time server is phase 12's deliberate design (it cannot
  be exhausted into spawning threads) and stays. Not reproduced in the browser runs.
- **MEDIUM, deferred to phase 15: `stratasoak` is not yet a soak test.** It discards what
  it reads, never reopens, and does not watch memory or descriptors. Phase 15 extends it
  with exactly those, which is what its release gate needs.
- **LOW, fixed:** the first interval after load or a reopen plotted the lifetime
  distribution as if it were the last second (now no point); an error before the first
  snapshot was hidden behind "connecting"; the window's midpoint read "−3m" at 2.5 minutes;
  a point between two gaps drew nothing; `--keys` above 10^12 would collide in the key's 12
  digits and an absurd `--value_size` would throw in a worker thread (both refused, and the
  CLI test checks it). A second Ctrl-C during shutdown ending the process is documented as
  intended, as in `stratactl serve`.

## What went wrong

- **The steps were written before they were compiled.** The phase 12 gates held the
  machine, so steps 1–4 were committed from a separate worktree and built afterwards. The
  first build found six problems, none subtle: CMake split each content type at its `;`
  (a list separator), a raw-string delimiter longer than the 16 characters C++ allows, an
  escaped `\n` that a scripted edit turned into a real newline in two files, GCC's
  `-Wmissing-field-initializers` on designated initializers, and a test helper that cut
  the response head one CRLF short. Lesson: an edit script that takes C++ or shell
  source as data must treat backslashes as data.
- **The first screenshot found what the unit tests could not.** The get latency line was
  invisible against write latency, and the I/O axis labels were clipped. Both are about
  what a person sees, and only looking showed them. The dashboard was checked in Chromium
  against a live `stratasoak` run at 1440 and 390 pixels wide, and its resource list
  in the browser was its own files and `/stats`, nothing else.
- **`pkill -f` killed its own shell** when restarting the demo, as it had before (see
  phase 11). The demo is now started with `exec` and stopped by its controlling task.
