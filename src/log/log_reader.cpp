#include "log/log_reader.hpp"

#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::log {
namespace {

// Whether some offset in `region` starts a complete record whose checksum
// verifies. A chance match needs a 32-bit checksum to agree, so a hit means
// real records follow. Only run on bytes about to be dropped.
bool HoldsValidRecord(Slice region) {
  for (std::size_t offset = 0; offset + kHeaderSize <= region.size(); ++offset) {
    const char* const header = region.data() + offset;
    const std::uint32_t length =
        static_cast<std::uint8_t>(header[4]) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[5])) << 8U);
    const auto type = static_cast<std::uint8_t>(header[6]);
    if (type == 0 || type > kMaxRecordType || kHeaderSize + length > region.size() - offset) {
      continue;
    }
    if (crc32c::Unmask(DecodeFixed32(header)) == crc32c::Value(Slice(header + 6, 1 + length))) {
      return true;
    }
  }
  return false;
}

}  // namespace

Reader::Reader(SequentialFile* file, Reporter* reporter)
    : file_(file), reporter_(reporter), backing_store_(std::make_unique<char[]>(kBlockSize)) {}

void Reader::ReportDrop(std::size_t bytes, Slice reason) {
  if (reporter_ != nullptr) {
    reporter_->Corruption(bytes, Status::Corruption(reason));
  }
}

void Reader::DropRestOfBlock(Slice reason) {
  // Skip the damaged header itself: its own bytes prove nothing.
  if (HoldsValidRecord(Slice(buffer_.data() + 1, buffer_.size() - 1))) {
    skipped_valid_record_ = true;
  }
  const std::size_t dropped = buffer_.size();
  buffer_ = Slice();
  ReportDrop(dropped, reason);
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  scratch->clear();
  *record = Slice();
  bool in_fragmented_record = false;
  std::uint64_t prospective_record_offset = 0;

  while (true) {
    Slice fragment;
    const unsigned outcome = ReadPhysicalRecord(&fragment);
    // Offset of this fragment's header: the buffer has already been advanced
    // past the header and payload just returned.
    const std::uint64_t physical_offset =
        end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

    switch (outcome) {
      case static_cast<unsigned>(RecordType::kFull):
        if (in_fragmented_record) {
          ReportDrop(scratch->size(), "partial record without end");
        }
        scratch->clear();
        *record = fragment;
        last_record_offset_ = physical_offset;
        return true;

      case static_cast<unsigned>(RecordType::kFirst):
        if (in_fragmented_record) {
          ReportDrop(scratch->size(), "partial record without end");
        }
        prospective_record_offset = physical_offset;
        scratch->assign(fragment);
        in_fragmented_record = true;
        break;

      case static_cast<unsigned>(RecordType::kMiddle):
        if (!in_fragmented_record) {
          ReportDrop(fragment.size(), "missing start of fragmented record");
        } else {
          scratch->append(fragment);
        }
        break;

      case static_cast<unsigned>(RecordType::kLast):
        if (!in_fragmented_record) {
          ReportDrop(fragment.size(), "missing start of fragmented record");
        } else {
          scratch->append(fragment);
          *record = *scratch;
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;

      case kEof:
        // A record cut off by the end of the file is a torn write: the writer
        // died before finishing it, so it was never acknowledged. Not
        // corruption, and not reported.
        scratch->clear();
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportDrop(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default:
        ReportDrop(fragment.size() + (in_fragmented_record ? scratch->size() : 0),
                   "unknown record type");
        in_fragmented_record = false;
        scratch->clear();
        break;
    }
  }
}

unsigned Reader::ReadPhysicalRecord(Slice* fragment) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (eof_) {
        // Fewer bytes than a header at the very end: a torn header.
        buffer_ = Slice();
        return kEof;
      }
      // Whatever is left in this block is a zero trailer; move to the next.
      buffer_ = Slice();
      auto read = file_->Read(kBlockSize, backing_store_.get());
      if (!read.ok()) {
        // Reported as the I/O error it is, not relabelled as corruption.
        if (reporter_ != nullptr) {
          reporter_->Corruption(kBlockSize, read.status());
        }
        eof_ = true;
        return kEof;
      }
      buffer_ = *read;
      end_of_buffer_offset_ += buffer_.size();
      if (buffer_.size() < kBlockSize) {
        eof_ = true;
      }
      continue;
    }

    const char* const header = buffer_.data();
    const std::uint32_t length =
        static_cast<std::uint8_t>(header[4]) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[5])) << 8U);
    const auto type = static_cast<std::uint8_t>(header[6]);

    if (kHeaderSize + length > buffer_.size()) {
      if (eof_ && !HoldsValidRecord(Slice(header + 1, buffer_.size() - 1))) {
        // The payload runs past the end of the file with nothing valid after
        // it: a torn write, not corruption -- see ReadRecord's kEof case. A
        // valid record after it means the length field itself is damaged.
        buffer_ = Slice();
        return kEof;
      }
      DropRestOfBlock("bad record length");
      return kBadRecord;
    }

    if (type == static_cast<std::uint8_t>(RecordType::kZero) && length == 0) {
      // Zero-filled space (e.g. preallocation) contains no records and is
      // skipped silently -- unless records follow, so the header was erased.
      if (HoldsValidRecord(Slice(header + 1, buffer_.size() - 1))) {
        DropRestOfBlock("zeroed header followed by records");
      } else {
        buffer_ = Slice();
      }
      return kBadRecord;
    }

    // The checksum covers the type byte and the payload.
    const std::uint32_t expected = crc32c::Unmask(DecodeFixed32(header));
    const std::uint32_t actual = crc32c::Value(Slice(header + 6, 1 + length));
    if (actual != expected) {
      // The length field is unverified, so following it could land mid-payload
      // and misread arbitrary bytes as a header. Drop the rest of the block and
      // resynchronise at the next block boundary.
      DropRestOfBlock("checksum mismatch");
      return kBadRecord;
    }

    buffer_.remove_prefix(kHeaderSize + length);
    *fragment = Slice(header + kHeaderSize, length);
    return type;
  }
}

}  // namespace strata::log
