#pragma once

#include "strata/slice.hpp"
#include "strata/status.hpp"
#include "strata/write_batch.hpp"

#include <cstddef>
#include <cstdint>

#include "db/dbformat.hpp"

namespace strata {

// Engine-only access to a WriteBatch's encoding: the header fields and the raw
// bytes that go into a WAL record. Kept out of the public header so the batch
// format stays an implementation detail users cannot depend on.
class WriteBatchInternal {
 public:
  // sequence (fixed64) + count (fixed32)
  static constexpr std::size_t kHeaderSize = 12;

  [[nodiscard]] static std::uint32_t Count(const WriteBatch& batch) noexcept;
  static void SetCount(WriteBatch* batch, std::uint32_t count) noexcept;

  // Sequence number of the batch's first operation.
  [[nodiscard]] static SequenceNumber Sequence(const WriteBatch& batch) noexcept;
  static void SetSequence(WriteBatch* batch, SequenceNumber sequence) noexcept;

  // The exact bytes to write as one WAL record.
  [[nodiscard]] static Slice Contents(const WriteBatch& batch) noexcept { return batch.rep_; }

  // Replaces the batch with bytes read from a WAL record. Only the header size
  // is checked here; Iterate validates the operations.
  static Status SetContents(WriteBatch* batch, Slice contents);
};

}  // namespace strata
