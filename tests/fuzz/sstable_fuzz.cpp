// Fuzz target: open arbitrary bytes as an SSTable and read all of it, forward,
// backward, and by seeks. Checks memory safety (ASan/libFuzzer) and that
// iteration always terminates.

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "support/string_file.hpp"
#include "table/table.hpp"
#include "table/table_builder.hpp"
#include "util/comparator.hpp"

namespace {

strata::table::TableOptions Options() {
  return {.comparator = strata::BytewiseComparator(), .block_size = 64};
}

std::string BuildTable(int entries) {
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::table::TableBuilder builder(Options(), &sink);
  for (int index = 0; index < entries; ++index) {
    static_cast<void>(builder.Add("key" + std::to_string(1000 + index),
                                  std::string(static_cast<std::size_t>(index % 17), 'v')));
  }
  static_cast<void>(builder.Finish());
  return contents;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::string bytes(reinterpret_cast<const char*>(data), size);
  auto table = strata::table::Table::Open(
      Options(), std::make_unique<strata::test::StringRandomAccessFile>(bytes), bytes.size());
  if (!table.ok()) {
    return 0;
  }
  auto it = (*table)->NewIterator();
  std::size_t steps = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (++steps > size) {
      std::abort();  // more entries than bytes: iteration is not terminating
    }
  }
  for (it->SeekToLast(); it->Valid() && steps < 2 * size + 2; it->Prev()) {
    ++steps;
  }
  it->Seek("key1005");
  static_cast<void>((*table)->Seek("key1", [](strata::Slice, strata::Slice) {}));
  static_cast<void>((*table)->ApproximateOffsetOf("key2"));
  return 0;
}

std::vector<std::string> StrataFuzzSeeds() {
  return {BuildTable(0), BuildTable(3), BuildTable(40)};
}
