#include "log/log_writer.hpp"

#include <algorithm>

#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::log {

Writer::Writer(WritableFile* destination, std::uint64_t existing_length)
    : destination_(destination), block_offset_(existing_length % kBlockSize) {
  for (std::uint8_t type = 0; type <= kMaxRecordType; ++type) {
    const char byte = static_cast<char>(type);
    type_crc_[type] = crc32c::Value(Slice(&byte, 1));
  }
}

Status Writer::AddRecord(Slice record) {
  STRATA_RETURN_IF_ERROR(error_);

  // Emit fragments until the record is used up. The do-while matters: an empty
  // record still produces one kFull fragment with no payload.
  bool first = true;
  do {
    const std::size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      // No header fits in the rest of this block: pad it with zeros (which the
      // reader skips) and continue in the next block.
      if (leftover > 0) {
        static constexpr char kZeros[kHeaderSize] = {};
        Status padded = destination_->Append(Slice(kZeros, leftover));
        if (!padded.ok()) {
          error_ = padded;
          return padded;
        }
      }
      block_offset_ = 0;
    }

    const std::size_t available = kBlockSize - block_offset_ - kHeaderSize;
    const std::size_t fragment_length = std::min(record.size(), available);
    const bool last = fragment_length == record.size();

    RecordType type = RecordType::kMiddle;
    if (first && last) {
      type = RecordType::kFull;
    } else if (first) {
      type = RecordType::kFirst;
    } else if (last) {
      type = RecordType::kLast;
    }

    Status emitted = EmitFragment(type, record.substr(0, fragment_length));
    if (!emitted.ok()) {
      error_ = emitted;
      return emitted;
    }
    record.remove_prefix(fragment_length);
    first = false;
  } while (!record.empty());

  Status flushed = destination_->Flush();
  if (!flushed.ok()) {
    error_ = flushed;
  }
  return flushed;
}

Status Writer::EmitFragment(RecordType type, Slice payload) {
  const auto type_index = static_cast<std::uint8_t>(type);
  std::array<char, kHeaderSize> header{};
  const std::uint32_t crc = crc32c::Extend(type_crc_[type_index], payload);
  EncodeFixed32(header.data(), crc32c::Mask(crc));
  header[4] = static_cast<char>(payload.size() & 0xFFU);
  header[5] = static_cast<char>(payload.size() >> 8U);
  header[6] = static_cast<char>(type_index);

  STRATA_RETURN_IF_ERROR(destination_->Append(Slice(header.data(), header.size())));
  STRATA_RETURN_IF_ERROR(destination_->Append(payload));
  block_offset_ += kHeaderSize + payload.size();
  return Status::Ok();
}

}  // namespace strata::log
