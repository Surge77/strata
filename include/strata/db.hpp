#pragma once

#include "strata/options.hpp"
#include "strata/result.hpp"
#include "strata/slice.hpp"
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
// and every operation on it returns InvalidArgument. All operations are safe to
// call from multiple threads at once.
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

  // Releases the database. Pending writes were already flushed to the
  // operating system when they returned; Close additionally releases the LOCK
  // so another process can open the database. Closing a closed DB is a no-op.
  Status Close();

 private:
  std::unique_ptr<DBImpl> impl_;
};

}  // namespace strata
