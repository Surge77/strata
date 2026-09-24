#pragma once

#include <string>

namespace strata {

class Env;
class Logger;

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

// Per-read options. Snapshots arrive in phase 7.
struct ReadOptions {};

}  // namespace strata
