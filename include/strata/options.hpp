#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace strata {

class Env;
class Logger;
class Snapshot;

// How to open a database. Designed for designated initialisers:
//
//   strata::DB::Open({.path = "./data", .create_if_missing = true}, &db);
struct Options {
  // Directory holding the database. Created if missing (see below).
  std::string path;

  // Create the directory and an empty database if none exists.
  bool create_if_missing = true;

  // Fail with AlreadyExists if a database is already at `path`.
  bool error_if_exists = false;

  // Damage inside the write-ahead log makes Open fail with Corruption, so an
  // acknowledged write is never dropped silently (ADR-0005). Set this to skip
  // damaged records instead -- each is logged -- when salvaging a database.
  // A torn record at the very end of the log is never corruption: it is what
  // a crash leaves behind, and it is always discarded.
  bool ignore_wal_corruption = false;

  // Bytes of writes buffered in memory before they are flushed to an SSTable.
  // Larger means fewer, bigger tables and more to replay after a crash.
  std::size_t write_buffer_size = 4 * 1024 * 1024;

  // Write a Bloom filter (about 10 bits per key) into each new SSTable, so a
  // lookup for a missing key usually skips that table without reading it.
  // Tables written either way stay readable whatever this is set to.
  bool bloom_filter = true;

  // SSTables kept open at once. Each open table holds a file descriptor and
  // its index and filter in memory. Reads stay correct beyond this limit; the
  // least recently used table is closed and reopened when next needed. 0
  // keeps none open, so every lookup reopens its table.
  std::size_t max_open_tables = 1000;

  // Bytes of data blocks cached in memory, shared by all tables. A hit skips
  // the file read and the checksum check. 0 disables the cache.
  std::size_t block_cache_size = 8 * 1024 * 1024;

  // Compaction cuts its output into tables of about this many bytes. A table
  // can run over when one key has many versions held by snapshots, because
  // all versions of a key always stay in one table.
  std::size_t max_file_size = 2 * 1024 * 1024;

  // Bytes level 1 may hold before compaction moves data into level 2. Each
  // deeper level may hold 10 times more than the one above it.
  std::uint64_t max_bytes_for_level_base = 10 * 1024 * 1024;

  // Filesystem access. Null means Env::Default(). Tests pass a
  // FaultInjectionEnv here to simulate crashes. Must outlive the DB.
  Env* env = nullptr;

  // Where diagnostics go. Null means a file named LOG inside `path`. Must
  // outlive the DB.
  Logger* info_log = nullptr;
};

// Per-write options. The default is `sync = false`; see ADR-0005 and
// docs/durability.md for exactly what that does and does not guarantee.
struct WriteOptions {
  // false: the write survives a crash of this *process* (it has reached the
  //        operating system before the call returns), but not power loss.
  // true:  the write survives power loss (fdatasync before returning).
  bool sync = false;
};

// Per-read options.
struct ReadOptions {
  // Read the database as it was when this snapshot was taken (see
  // DB::GetSnapshot). Null reads the latest state.
  const Snapshot* snapshot = nullptr;

  // For iterators only: keys below `lower_bound` (inclusive) and at or above
  // `upper_bound` (exclusive) are outside the scan. Unset means unbounded.
  std::optional<std::string> lower_bound = std::nullopt;
  std::optional<std::string> upper_bound = std::nullopt;
};

}  // namespace strata
