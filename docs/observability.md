# Observability

What a running strata database tells you about itself, and how to read it:

- **`DB::GetStats()`** returns a `strata::Stats` (`include/strata/stats.hpp`): counters,
  gauges, latency percentiles, the LSM levels, the running compaction, and recent events.
- **`StatsToJson()`** renders it as JSON, the schema below.
- **`StatsServer`** (`include/strata/stats_server.hpp`) serves that JSON over HTTP.
  `stratactl serve <db>` runs one; `stratactl stats <db>` prints one snapshot.
- **The info log** (`LOG` in the database directory) gets one `key=value` line per event.

`stratactl stats` and `serve` open the database themselves, read-write, like any other
process. That has three consequences. They cannot attach to a database another process
has open (the `LOCK` file refuses them). Their counters start from their own Open, so
`stats` mostly prints zeros and what recovery did. And opening is not inspection: it
replays the write-ahead log, flushes it to a table, deletes old logs, and may start a
compaction, so do not use them on a damaged database you want to examine as it is. To
watch a running engine, start a `StatsServer` inside the process that owns it.

Taking a snapshot does no I/O and holds the database mutex only long enough to copy a
few numbers, so polling it once a second costs nothing measurable.

## Metrics

Names are stable: renaming one is a breaking change. Counters only grow from the moment
the database is opened; a rate is the difference between two snapshots divided by the
time between them (`unix_micros`). Gauges are current values.

### Counters

| Name | Counts |
|---|---|
| `get.calls` | `Get` calls that reached a lookup (not those refused for an oversized key, a closed database, or a released or foreign snapshot) |
| `get.found` | ...of which found a value |
| `get.memtable_answers` | ...answered by the active or immutable memtable, found or deleted, without touching a table |
| `get.table_reads` | tables a `Get` searched past the Bloom filter (each one a block lookup, cached or not) |
| `get.filter_skips` | tables a `Get` skipped because the Bloom filter ruled the key out |
| `write.calls` | `Put`, `Delete`, and `Write` calls with at least one operation that succeeded (a refused or failed write is not counted) |
| `write.groups` | log records written: one per commit group, so `write.calls / write.groups` is the group-commit factor |
| `write.bytes` | encoded batch bytes accepted; the denominator of write amplification |
| `write.syncs` | log fsyncs paid for synced groups |
| `write.slowdowns` | writes delayed 1 ms because level 0 was long |
| `write.stops` | waits because level 0 was full |
| `log.rotations` | memtable switches, each to a new write-ahead log |
| `flush.count`, `flush.bytes` | memtables flushed to level-0 tables, and the bytes written |
| `compaction.count` | merging compactions (not trivial moves) |
| `compaction.trivial_moves` | tables relinked one level down without rewriting |
| `compaction.bytes_read`, `compaction.bytes_written` | table bytes merged and produced |
| `block_cache.hits`, `block_cache.misses` | data block lookups in the shared block cache |
| `table_cache.hits`, `table_cache.misses` | lookups of an open table; a miss opens the file |
| `iterator.created` | `NewIterator` calls |

Write amplification is `(flush.bytes + compaction.bytes_written) / write.bytes`.

### Gauges

| Name | Value |
|---|---|
| `memtable.bytes` | memory used by the active memtable |
| `immutable_memtable.bytes` | memory used by the memtable being flushed; 0 if none |
| `sequence.last` | the newest sequence number: writes applied since the database was created |
| `snapshots.live` | snapshots not yet released |
| `writers.queued` | writes waiting behind the commit group leader |
| `block_cache.bytes`, `block_cache.capacity_bytes` | block cache usage and limit |
| `table_cache.tables`, `table_cache.capacity` | open tables and the limit (`max_open_tables`) |

### Latencies

`get` and `write`, in microseconds (fractional), from the call to its return. Each times
one call in 16 on each thread, so its `count` is the number of calls timed, about
`get.calls / 16` or `write.calls / 16`. Two clock reads cost more than 10% of the cheapest
`Get` (a miss ruled out by every Bloom filter) and about 4% of an unsynced write. Every
thread starts counting at a different offset, so neither a thread's cold first call nor
one call number shared by all threads is favoured. It is still a systematic sample: a
thread whose own calls repeat a pattern every 16 (one slow call in each 16, say) can be
timed always or never on that call.
`write` includes the time spent queued behind other writers and any stall, because that
is what the caller waited. Each reports `count`, `mean`, `p50`, `p90`, `p99`, `p999`, and
`max` since Open. Samples are taken in nanoseconds, so a sub-microsecond memtable `Get`
is not rounded to 0. Percentiles come from the same log-linear buckets as `Histogram`
and are within 6.25% of the true value.

## Events

| Event | Level | Fields |
|---|---|---|
| `db_opening` | INFO | `path` |
| `wal_replayed` | INFO | `log`, `batches` |
| `wal_corruption_skipped` | WARN | `file`, `bytes`, `reason`: a damaged log record dropped during replay |
| `wal_torn_tail` | WARN | `file`: damage at the very end of the newest log, treated as an interrupted write |
| `db_opened` | INFO | `logs_replayed`, `last_sequence` |
| `log_rotated` | INFO | `log` (the new one) |
| `flush_finished` | INFO | `table`, `bytes`, `micros` |
| `table_moved` | INFO | `table`, `level` (the destination) |
| `compaction_finished` | INFO | `level`, `output_level`, `bytes_read`, `bytes_written`, `micros` |
| `write_failed`, `log_rotation_failed`, `flush_failed`, `compaction_failed` | ERROR | `log` or nothing, `error`; each leaves the database refusing writes |
| `db_closed` | INFO | none |

Events recorded while `Open` runs are kept as `recovery_events` for the life of the
database (up to 256), so what recovery did stays visible after hours of routine flushes.
Every event also enters `recent_events`, a ring of the last 256.

In the info log each event is one line after the usual timestamp, level, and thread id:

```text
2026-10-27T10:15:30.123456Z INFO  [4711] event=flush_finished table=12 bytes=40960 micros=812
```

Text values are quoted when they are empty or contain a space, `=`, `"`, `\`, or a control
character, so an event is always exactly one line and splits on spaces outside quotes.

## JSON schema

```jsonc
{
  "unix_micros": 1793095000000000,   // when the snapshot was taken
  "uptime_micros": 5000000,          // since Open
  "counters": { "get.calls": 15, ... },
  "gauges": { "memtable.bytes": 4096, ... },
  "latencies": {
    "get":   { "count": 15, "mean": 1.9, "p50": 1.4, "p90": 2.8, "p99": 7.5, "p999": 7.9, "max": 8 },
    "write": { ... }
  },
  "levels": [   // one per level, 0 to 6
    { "level": 0, "files": 2, "bytes": 81920, "target_bytes": 0, "compaction_score": 0.5 }
  ],
  "compaction": { "running": true, "level": 1, "input_bytes": 6291456, "bytes_written": 2097152 },
  "recovery_events": [
    { "sequence": 1, "unix_micros": 1793094995000000, "level": "INFO",
      "name": "db_opening", "fields": { "path": "/data/db" } }
  ],
  "recent_events": [ ... ],          // oldest first
  "background_error": null           // or the error that stopped writes
}
```

Numbers in `fields` are JSON numbers; text is a JSON string. Strings are always valid
UTF-8: any byte that is not part of well-formed UTF-8 (a file name can contain anything)
becomes U+FFFD. Counters are 64-bit; JavaScript reads integers exactly only up to 2^53,
which a byte counter reaches after 9 petabytes.

## The HTTP endpoint

| Request | Response |
|---|---|
| `GET /stats` | 200 with the JSON above; 503 if the database is closed |
| `GET /healthz` | 200 `ok`; 503 once the database refuses writes (the body says why) |
| `HEAD` of either | the same status and headers, no body |
| any other path | 404 |
| any other uppercase method | 405, with `Allow: GET, HEAD` |
| a method with lowercase letters or `-` (`get`, `M-SEARCH`) | 400: the parser accepts only `A`–`Z` |
| HTTP/1.1 without `Host` | 400 (HTTP/1.0 may omit it) |

It is deliberately small. Requests are served one at a time on one thread. One 2-second
deadline covers the whole exchange, reading the request and sending the response, and the
request head must fit in 8 KiB (else 408 or 431; the short 408 reply gets its own 2
seconds). A stuck or hostile client, including one that reads the response a byte at a
time, therefore delays others by at most about 2 seconds (4 for a request that times out).
Every response closes the connection.

**Lifetime.** The server calls the database from its own thread, so the usual rule
applies: stop the server (`Stop()` or its destructor) before closing, destroying,
reopening, or moving the `DB` it serves.

**Security model.** The server only reads; nothing it serves can change the database.
What it reveals is operational: counters, file numbers, the database path, and error
messages. Keys and values are never included.

- It binds `127.0.0.1` by default, so only processes on the same machine can connect.
- On a loopback address (including `::1` and `::ffff:127.0.0.1`) it also refuses (403) any request whose `Host` header is not
  `localhost`, `127.0.0.1`, or `[::1]`. Without that check, a web page could rebind its
  own domain name to 127.0.0.1 and read the endpoint through the visitor's browser.
  Binding another 127.x address works, but then no Host passes the check; use
  `127.0.0.1`.
- It sends no CORS headers, so a page on another origin cannot read a response.
- Binding any other address (`--bind 0.0.0.0`) serves everyone who can reach the port,
  with no authentication and no TLS, and `stratactl serve` warns when it does. Do that
  only behind a proxy that provides both.
- A 503 body carries the engine's error text, which can include the database path.
