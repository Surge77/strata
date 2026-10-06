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

namespace {

// A key too short to hold a trailer is malformed. Ordering it by its bytes, as
// if its trailer were zero, keeps every comparison inside the key; whoever
// parses the key then reports Corruption.
Slice UserKeyOrWhole(Slice key) {
  return key.size() >= kInternalKeyTrailerSize ? ExtractUserKey(key) : key;
}
std::uint64_t TrailerOrZero(Slice key) {
  return key.size() >= kInternalKeyTrailerSize
             ? DecodeFixed64(key.data() + key.size() - kInternalKeyTrailerSize)
             : 0;
}

}  // namespace

int InternalKeyComparator::Compare(Slice a, Slice b) const {
  const int by_user_key = user_comparator_->Compare(UserKeyOrWhole(a), UserKeyOrWhole(b));
  if (by_user_key != 0) {
    return by_user_key;
  }
  const std::uint64_t trailer_a = TrailerOrZero(a);
  const std::uint64_t trailer_b = TrailerOrZero(b);
  // Descending: the larger trailer (newer sequence) sorts first.
  if (trailer_a > trailer_b) {
    return -1;
  }
  if (trailer_a < trailer_b) {
    return 1;
  }
  return 0;
}

void InternalKeyComparator::FindShortestSeparator(std::string* start, Slice limit) const {
  const Slice user_start = ExtractUserKey(*start);
  const Slice user_limit = ExtractUserKey(limit);
  std::string shortened(user_start);
  user_comparator_->FindShortestSeparator(&shortened, user_limit);
  if (shortened.size() < user_start.size() &&
      user_comparator_->Compare(user_start, shortened) < 0) {
    // The user key got shorter and larger. The earliest possible trailer
    // (largest sequence) makes it sort before every entry of that user key.
    PutFixed64(&shortened, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    start->swap(shortened);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  const Slice user_key = ExtractUserKey(*key);
  std::string successor(user_key);
  user_comparator_->FindShortSuccessor(&successor);
  if (successor.size() < user_key.size() && user_comparator_->Compare(user_key, successor) < 0) {
    PutFixed64(&successor, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    key->swap(successor);
  }
}

LookupKey::LookupKey(Slice user_key, SequenceNumber sequence) {
  const std::size_t internal_key_size = user_key.size() + kInternalKeyTrailerSize;
  storage_.reserve(kMaxVarint32Length + internal_key_size);
  PutVarint32(&storage_, static_cast<std::uint32_t>(internal_key_size));
  internal_key_offset_ = storage_.size();
  AppendInternalKey(&storage_, {user_key, sequence, kValueTypeForSeek});
}

const InternalKeyComparator* BytewiseInternalKeyComparator() {
  static const InternalKeyComparator* const kInstance =
      new InternalKeyComparator(BytewiseComparator());
  return kInstance;
}

}  // namespace strata
