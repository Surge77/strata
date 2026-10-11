#include "stratabench/bench_db.hpp"

#include "strata/write_batch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "db/dbformat.hpp"
#include "util/random.hpp"

namespace strata::bench {
namespace {

constexpr std::uint64_t kDecimalBase = 10;
constexpr char kMissingSuffix = '.';
// Large enough that values drawn from it do not repeat in a cached block.
constexpr std::size_t kValuePoolBytes = 1 << 20;
constexpr char kFirstPrintable = ' ';
constexpr std::uint64_t kPrintableCount = 95;

}  // namespace

Slice FormatKey(std::uint64_t number, KeyBuffer* buffer) noexcept {
  for (std::size_t index = kKeySize; index > 0; --index) {
    buffer->bytes[index - 1] = static_cast<char>('0' + number % kDecimalBase);
    number /= kDecimalBase;
  }
  return {buffer->bytes, kKeySize};
}

Slice FormatMissingKey(std::uint64_t number, KeyBuffer* buffer) noexcept {
  static_cast<void>(FormatKey(number, buffer));
  buffer->bytes[kKeySize] = kMissingSuffix;
  return {buffer->bytes, kKeySize + 1};
}

ValueSource::ValueSource(std::size_t value_size, std::uint64_t seed)
    : data_(std::max(kValuePoolBytes, 2 * value_size), '\0'), value_size_(value_size) {
  Random random(seed);
  for (char& byte : data_) {
    byte = static_cast<char>(kFirstPrintable + random.Uniform(kPrintableCount));
  }
}

Slice ValueSource::At(std::uint64_t index) const noexcept {
  // The pool is at least twice the value size, so there is always a start.
  // Stride value_size + 1 keeps consecutive values from being aligned copies.
  const std::size_t starts = data_.size() - value_size_ + 1;
  const std::size_t offset = (index * (value_size_ + 1)) % starts;
  return {data_.data() + offset, value_size_};
}

void Fail(const char* what, const Status& status) {
  std::fprintf(stderr, "stratabench: %s: %s\n", what, status.ToString().c_str());
  std::exit(1);
}

std::unique_ptr<DBImpl> OpenExisting(const std::filesystem::path& path) {
  auto opened = DBImpl::Open({.path = path.string()});
  if (!opened.ok()) {
    Fail("open", opened.status());
  }
  return std::move(*opened);
}

std::unique_ptr<DBImpl> OpenFresh(const std::filesystem::path& path) {
  std::filesystem::remove_all(path);
  return OpenExisting(path);
}

void CloseOrFail(DBImpl* db) {
  if (const Status status = db->Close(); !status.ok()) {
    Fail("close", status);
  }
}

void EvictFromPageCache(const std::filesystem::path& path) {
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const int fd = ::open(entry.path().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      continue;  // removed by a compaction since the listing; nothing to evict
    }
    static_cast<void>(::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
    ::close(fd);
  }
}

LoadedDb::LoadedDb(std::filesystem::path path, std::uint64_t keys, std::size_t value_size,
                   std::uint64_t seed)
    : path_(std::move(path)), keys_(keys) {
  std::vector<std::uint64_t> order(keys);
  std::iota(order.begin(), order.end(), std::uint64_t{0});
  Random random(seed);
  for (std::size_t index = order.size(); index > 1; --index) {  // Fisher-Yates
    std::swap(order[index - 1], order[random.Uniform(index)]);
  }
  {
    auto db = OpenFresh(path_);
    ValueSource values(value_size, seed);
    KeyBuffer key;
    for (const std::uint64_t number : order) {
      WriteBatch batch;
      batch.Put(FormatKey(number, &key), values.At(number));
      if (const Status status = db->Write({}, batch); !status.ok()) {
        Fail("load", status);
      }
    }
    db->WaitForCompactionsForTest();
    CloseOrFail(db.get());
  }
  db_ = OpenExisting(path_);
  db_->WaitForCompactionsForTest();
}

void LoadedDb::PrepareRound(bool cold) {
  if (!cold) {
    return;
  }
  CloseOrFail(db_.get());
  db_.reset();
  EvictFromPageCache(path_);
  db_ = OpenExisting(path_);
  db_->WaitForCompactionsForTest();
}

std::string LoadedDb::Shape() const {
  int deepest = 0;
  for (int level = 0; level < kNumLevels; ++level) {
    if (!db_->FilesAtLevelForTest(level).empty()) {
      deepest = level;
    }
  }
  std::string shape;
  for (int level = 0; level <= deepest; ++level) {
    shape += (level == 0 ? "" : " ") + std::to_string(db_->FilesAtLevelForTest(level).size());
  }
  return shape;
}

}  // namespace strata::bench
