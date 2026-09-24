#include "db/dbformat.hpp"

#include "util/coding.hpp"

namespace strata {

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key);
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(Slice internal_key, ParsedInternalKey* result) noexcept {
  if (internal_key.size() < kInternalKeyTrailerSize) {
    return false;
  }
  const std::uint64_t trailer =
      DecodeFixed64(internal_key.data() + internal_key.size() - kInternalKeyTrailerSize);
  const auto type = static_cast<std::uint8_t>(trailer & 0xFFU);
  if (type > static_cast<std::uint8_t>(ValueType::kValue)) {
    return false;
  }
  result->user_key = ExtractUserKey(internal_key);
  result->sequence = trailer >> 8U;
  result->type = static_cast<ValueType>(type);
  return true;
}

int InternalKeyComparator::Compare(Slice a, Slice b) const noexcept {
  const int by_user_key = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (by_user_key != 0) {
    return by_user_key;
  }
  const std::uint64_t trailer_a = DecodeFixed64(a.data() + a.size() - kInternalKeyTrailerSize);
  const std::uint64_t trailer_b = DecodeFixed64(b.data() + b.size() - kInternalKeyTrailerSize);
  // Descending: the larger trailer (newer sequence) sorts first.
  if (trailer_a > trailer_b) {
    return -1;
  }
  if (trailer_a < trailer_b) {
    return 1;
  }
  return 0;
}

LookupKey::LookupKey(Slice user_key, SequenceNumber sequence) {
  const std::size_t internal_key_size = user_key.size() + kInternalKeyTrailerSize;
  storage_.reserve(kMaxVarint32Length + internal_key_size);
  PutVarint32(&storage_, static_cast<std::uint32_t>(internal_key_size));
  internal_key_offset_ = storage_.size();
  AppendInternalKey(&storage_, {user_key, sequence, kValueTypeForSeek});
}

}  // namespace strata
