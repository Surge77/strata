// Fuzz target: the WAL recovery input path -- log reader, then WriteBatch
// decoding of every record it returns -- on arbitrary bytes.
//
// Both parse untrusted input: a log file damaged by a disk, a bug, or an
// attacker. The properties checked are the ones recovery depends on:
//   * no crash, hang, or out-of-bounds access (ASan / libFuzzer catch those);
//   * the reader never returns more payload bytes than the input contains.

#include "strata/write_batch.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "db/write_batch_internal.hpp"
#include "log/log_reader.hpp"
#include "log/log_writer.hpp"
#include "support/string_file.hpp"

namespace {

class DropCounter final : public strata::log::Reader::Reporter {
 public:
  void Corruption(std::size_t bytes_dropped, const strata::Status& /*reason*/) override {
    dropped += bytes_dropped;
  }
  std::size_t dropped = 0;
};

class NullHandler final : public strata::WriteBatch::Handler {
 public:
  void Put(strata::Slice key, strata::Slice value) override { bytes += key.size() + value.size(); }
  void Delete(strata::Slice key) override { bytes += key.size(); }
  std::size_t bytes = 0;
};

std::string EncodeLog(const std::vector<std::string>& records) {
  std::string contents;
  strata::test::StringSink sink(&contents);
  strata::log::Writer writer(&sink);
  for (const auto& record : records) {
    static_cast<void>(writer.AddRecord(record));
  }
  return contents;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  strata::test::StringSource source(std::string(reinterpret_cast<const char*>(data), size));
  DropCounter reporter;
  strata::log::Reader reader(&source, &reporter);

  std::size_t payload_bytes = 0;
  strata::Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    payload_bytes += record.size();
    strata::WriteBatch batch;
    if (strata::WriteBatchInternal::SetContents(&batch, record).ok()) {
      NullHandler handler;
      static_cast<void>(batch.Iterate(&handler));
      if (handler.bytes > record.size()) {
        std::abort();  // decoded more key/value bytes than the record holds
      }
    }
  }
  if (payload_bytes > size) {
    std::abort();  // returned more than the input contained
  }
  return 0;
}

// Well-formed logs for the GCC driver to mutate (libFuzzer builds use a corpus
// directory instead): empty, small records, multi-block records, and records
// that are real encoded WriteBatches, so mutations reach the batch decoder.
std::vector<std::string> StrataFuzzSeeds() {
  strata::WriteBatch batch;
  batch.Put("key", "value");
  batch.Delete("gone");
  batch.Put(std::string(300, 'k'), std::string(5000, 'v'));
  const std::string encoded(strata::WriteBatchInternal::Contents(batch));

  return {
      EncodeLog({}),
      EncodeLog({"a", "", "bc"}),
      EncodeLog({encoded, encoded}),
      EncodeLog({std::string(40000, 'x'), "tail"}),
      EncodeLog({std::string(100000, 'y'), encoded}),
  };
}
