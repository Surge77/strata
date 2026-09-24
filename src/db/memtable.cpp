#include "db/memtable.hpp"

#include <cstring>

#include "util/coding.hpp"

namespace strata {
namespace {

// memcpy with a null source is undefined behaviour even for zero bytes, and an
// empty Slice (e.g. the value of a Delete) may well have a null data().
char* CopyBytes(char* destination, Slice bytes) {
  if (!bytes.empty()) {
    std::memcpy(destination, bytes.data(), bytes.size());
  }
  return destination + bytes.size();
}

// Reads a varint32-length-prefixed slice starting at `data`. Entries in the
// arena were written by Add, so they are trusted: no bounds are known or
// checked here.
Slice GetLengthPrefixed(const char* data) {
  Slice input(data, kMaxVarint32Length);
  std::uint32_t length = 0;
  static_cast<void>(GetVarint32(&input, &length));
  return {input.data(), length};
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator->Compare(GetLengthPrefixed(a), GetLengthPrefixed(b));
}

MemTable::MemTable(const InternalKeyComparator* comparator)
    : table_(KeyComparator{comparator}, &arena_) {}

void MemTable::Add(SequenceNumber sequence, ValueType type, Slice user_key, Slice value) {
  const std::size_t internal_key_size = user_key.size() + kInternalKeyTrailerSize;
  const std::size_t encoded_size =
      static_cast<std::size_t>(VarintLength(internal_key_size)) + internal_key_size +
      static_cast<std::size_t>(VarintLength(value.size())) + value.size();

  char* const entry = arena_.Allocate(encoded_size);
  char* cursor = EncodeVarint32(entry, static_cast<std::uint32_t>(internal_key_size));
  cursor = CopyBytes(cursor, user_key);
  EncodeFixed64(cursor, PackSequenceAndType(sequence, type));
  cursor += kInternalKeyTrailerSize;
  cursor = EncodeVarint32(cursor, static_cast<std::uint32_t>(value.size()));
  CopyBytes(cursor, value);

  table_.Insert(entry);
}

MemTable::LookupResult MemTable::Get(const LookupKey& key, std::string* value) const {
  Table::Iterator it(&table_);
  it.Seek(key.memtable_key().data());
  if (!it.Valid()) {
    return LookupResult::kNotPresent;
  }

  // The seek landed on the first entry at or after (user_key, sequence). It is
  // the answer only if it is for the same user key; the sequence ordering
  // already guarantees it is the newest version not newer than `key`.
  const Slice internal_key = GetLengthPrefixed(it.key());
  ParsedInternalKey parsed;
  if (!ParseInternalKey(internal_key, &parsed) || parsed.user_key != key.user_key()) {
    return LookupResult::kNotPresent;
  }
  if (parsed.type == ValueType::kDeletion) {
    return LookupResult::kDeleted;
  }
  const Slice stored = GetLengthPrefixed(internal_key.data() + internal_key.size());
  value->assign(stored.data(), stored.size());
  return LookupResult::kFound;
}

// Adapts the skiplist iterator to strata::Iterator, decoding entries on the
// fly. Seek takes an internal key and encodes it into the memtable's
// length-prefixed form in a scratch buffer.
class MemTable::MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const Table* table) : it_(table) {}

  [[nodiscard]] bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void SeekToLast() override { it_.SeekToLast(); }
  void Seek(Slice target) override {
    scratch_.clear();
    PutVarint32(&scratch_, static_cast<std::uint32_t>(target.size()));
    scratch_.append(target);
    it_.Seek(scratch_.data());
  }
  void Next() override { it_.Next(); }
  void Prev() override { it_.Prev(); }
  [[nodiscard]] Slice key() const override { return GetLengthPrefixed(it_.key()); }
  [[nodiscard]] Slice value() const override {
    const Slice internal_key = key();
    return GetLengthPrefixed(internal_key.data() + internal_key.size());
  }
  [[nodiscard]] Status status() const override { return Status::Ok(); }

 private:
  Table::Iterator it_;
  std::string scratch_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace strata
