#pragma once

#include "strata/slice.hpp"
#include "strata/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace strata {

// An ordered group of Put and Delete operations applied atomically: after a
// crash, either every operation in the batch is recovered or none is.
//
// The atomicity comes from the encoding (docs/format/write_batch.md): the whole
// batch is serialised into one WAL record, and a log record is either read back
// whole or not at all.
//
// Operations apply in the order they were added, so a Put followed by a Delete
// of the same key leaves the key deleted. Not thread-safe; build a batch on one
// thread, then hand it to the database.
class WriteBatch {
 public:
  // Every length in the engine's formats is a varint32, so keys (which gain an
  // 8-byte internal trailer) and values are limited to fit one. An operation
  // that exceeds a limit is not recorded; it marks the batch oversized, and
  // DB::Write rejects the whole batch with InvalidArgument. Nothing is ever
  // silently truncated.
  static constexpr std::size_t kMaxKeySize = 0xFFFFFFFFU - 8U;
  static constexpr std::size_t kMaxValueSize = 0xFFFFFFFFU;

  WriteBatch();

  void Put(Slice key, Slice value);
  void Delete(Slice key);

  // Removes every operation (and clears the oversized mark).
  void Clear();

  // Adds every operation of `other` after this batch's operations.
  void Append(const WriteBatch& other);

  [[nodiscard]] std::uint32_t Count() const noexcept;

  // True if any Put or Delete exceeded kMaxKeySize or kMaxValueSize, including
  // one appended from another batch.
  [[nodiscard]] bool oversized() const noexcept { return oversized_; }

  // Bytes the batch occupies in the WAL, header included.
  [[nodiscard]] std::size_t ApproximateSize() const noexcept { return rep_.size(); }

  // Receives the operations of a batch, in order.
  class Handler {
   public:
    Handler() = default;
    virtual ~Handler() = default;
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;

    virtual void Put(Slice key, Slice value) = 0;
    virtual void Delete(Slice key) = 0;
  };

  // Calls the handler once per operation. Returns Corruption, having delivered
  // only the operations before the damage, if the encoding is malformed --
  // which can only happen to a batch decoded from bytes (a WAL record), never to
  // one built with Put and Delete.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;
  bool oversized_ = false;
};

}  // namespace strata
