#pragma once

#include "strata/iterator.hpp"
#include "strata/options.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
#include "strata/snapshot.hpp"
#include "strata/status.hpp"
#include "strata/write_batch.hpp"

#include <memory>
#include <string>

namespace strata {

class DBImpl;

// An open database: a persistent, sorted map from byte-string keys to
// byte-string values.
//
//   strata::DB db;
//   strata::Status s = strata::DB::Open({.path = "./data"}, &db);
//   db.Put("user:101", "rahul");
//   strata::Result<std::string> value = db.Get("user:101");   // "rahul"
//   db.Delete("user:101");
//
// A DB is a move-only handle. Default-constructed or moved-from, it is closed,
// and every operation on it returns InvalidArgument.
//
// Put, Delete, Write, and Get are safe to call from many threads at once.
// Close, destruction, and move-assignment are not: like destroying any C++
// object, they must not race with other calls on the same DB. (The engine
// still refuses cleanly if a call was already waiting when Close finished.)
//
// What a successful write guarantees -- process crash versus power loss -- is
// set by WriteOptions::sync; see docs/durability.md.
class DB {
 public:
  DB();
  ~DB();
  DB(DB&& other) noexcept;
  DB& operator=(DB&& other) noexcept;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Opens (and, per options, creates) the database at options.path, replaying
  // its write-ahead log. `*db` must be closed; on failure it stays closed.
  // Returns Busy if the database is already open, in this process or another.
  static Status Open(const Options& options, DB* db);

  [[nodiscard]] bool is_open() const noexcept { return impl_ != nullptr; }

  // Sets `key` to `value`. Keys may be up to 2^32 - 9 bytes and values up to
  // 2^32 - 1; larger ones are rejected with InvalidArgument, never truncated.
  Status Put(Slice key, Slice value, const WriteOptions& options = {});

  // Removes `key`. Deleting an absent key is not an error.
  Status Delete(Slice key, const WriteOptions& options = {});

  // Applies every operation in `batch` atomically: after a crash, all of them
  // are recovered or none are.
  Status Write(const WriteBatch& batch, const WriteOptions& options = {});

  // The value of `key`, or NotFound if it is absent or deleted.
  [[nodiscard]] Result<std::string> Get(Slice key, const ReadOptions& options = {}) const;

  // An iterator over the database in key order, as of now (or as of
  // options.snapshot), limited to [options.lower_bound, options.upper_bound).
  // It is a consistent view: writes, deletes, and compactions after it was
  // created do not change what it shows. It stays usable after the snapshot it
  // used is released and after this DB is closed, as long as Options::env
  // outlives it -- but tables below level 0 are opened only when the scan
  // reaches them, so if the database is reopened and compacted meanwhile, the
  // scan can stop with an error. A closed database returns an empty iterator
  // whose status() is InvalidArgument. Each live iterator keeps the tables it
  // has opened open, beyond Options::max_open_tables. Not thread-safe: one
  // thread per iterator.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(const ReadOptions& options = {}) const;

  // A snapshot of the current state, for ReadOptions::snapshot. Release it
  // with ReleaseSnapshot before closing the database. Null if the database is
  // closed.
  [[nodiscard]] const Snapshot* GetSnapshot();

  // Gives back a snapshot from GetSnapshot. InvalidArgument for a null,
  // already released, or foreign snapshot.
  Status ReleaseSnapshot(const Snapshot* snapshot);

  // Syncs the write-ahead log -- so after a successful Close every write is
  // durable against power loss, whatever its WriteOptions -- then releases
  // the LOCK so another process can open the database. Closing a closed DB is
  // a no-op.
  Status Close();

 private:
  std::unique_ptr<DBImpl> impl_;
};

}  // namespace strata
