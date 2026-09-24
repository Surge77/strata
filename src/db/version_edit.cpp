#include "db/version_edit.hpp"

#include "util/coding.hpp"

namespace strata {
namespace {

enum Tag : std::uint32_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kDeletedFile = 5,
  kNewFile = 6,
};

bool GetLevel(Slice* input, int* level) {
  std::uint32_t value = 0;
  if (!GetVarint32(input, &value) || value >= static_cast<std::uint32_t>(kNumLevels)) {
    return false;
  }
  *level = static_cast<int>(value);
  return true;
}

bool GetInternalKey(Slice* input, std::string* key) {
  Slice encoded;
  ParsedInternalKey parsed;
  if (!GetLengthPrefixedSlice(input, &encoded) || !ParseInternalKey(encoded, &parsed)) {
    return false;
  }
  key->assign(encoded);
  return true;
}

}  // namespace

void VersionEdit::EncodeTo(std::string* destination) const {
  if (comparator_) {
    PutVarint32(destination, kComparator);
    PutLengthPrefixedSlice(destination, *comparator_);
  }
  if (log_number_) {
    PutVarint32(destination, kLogNumber);
    PutVarint64(destination, *log_number_);
  }
  if (next_file_number_) {
    PutVarint32(destination, kNextFileNumber);
    PutVarint64(destination, *next_file_number_);
  }
  if (last_sequence_) {
    PutVarint32(destination, kLastSequence);
    PutVarint64(destination, *last_sequence_);
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(destination, kDeletedFile);
    PutVarint32(destination, static_cast<std::uint32_t>(level));
    PutVarint64(destination, number);
  }
  for (const auto& [level, file] : new_files_) {
    PutVarint32(destination, kNewFile);
    PutVarint32(destination, static_cast<std::uint32_t>(level));
    PutVarint64(destination, file.number);
    PutVarint64(destination, file.file_size);
    PutLengthPrefixedSlice(destination, file.smallest);
    PutLengthPrefixedSlice(destination, file.largest);
  }
}

Status VersionEdit::DecodeFrom(Slice input) {
  *this = VersionEdit();
  while (!input.empty()) {
    std::uint32_t tag = 0;
    if (!GetVarint32(&input, &tag)) {
      return Status::Corruption("version edit: bad tag");
    }
    std::uint64_t number = 0;
    int level = 0;
    switch (tag) {
      case kComparator: {
        Slice name;
        if (!GetLengthPrefixedSlice(&input, &name)) {
          return Status::Corruption("version edit: bad comparator name");
        }
        comparator_ = std::string(name);
        break;
      }
      case kLogNumber:
        if (!GetVarint64(&input, &number)) {
          return Status::Corruption("version edit: bad log number");
        }
        log_number_ = number;
        break;
      case kNextFileNumber:
        if (!GetVarint64(&input, &number)) {
          return Status::Corruption("version edit: bad next file number");
        }
        next_file_number_ = number;
        break;
      case kLastSequence:
        if (!GetVarint64(&input, &number) || number > kMaxSequenceNumber) {
          return Status::Corruption("version edit: bad last sequence");
        }
        last_sequence_ = number;
        break;
      case kDeletedFile:
        if (!GetLevel(&input, &level) || !GetVarint64(&input, &number)) {
          return Status::Corruption("version edit: bad deleted file");
        }
        deleted_files_.emplace(level, number);
        break;
      case kNewFile: {
        FileMetaData file;
        if (!GetLevel(&input, &level) || !GetVarint64(&input, &file.number) ||
            !GetVarint64(&input, &file.file_size) || !GetInternalKey(&input, &file.smallest) ||
            !GetInternalKey(&input, &file.largest)) {
          return Status::Corruption("version edit: bad new file");
        }
        new_files_.emplace_back(level, std::move(file));
        break;
      }
      default:
        return Status::Corruption("version edit: unknown tag");
    }
  }
  return Status::Ok();
}

}  // namespace strata
