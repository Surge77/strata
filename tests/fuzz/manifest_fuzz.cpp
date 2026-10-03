// Fuzz the MANIFEST input path: framed log records and version edits.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "db/version_edit.hpp"
#include "log/log_reader.hpp"
#include "log/log_writer.hpp"
#include "support/string_file.hpp"

namespace {

class Reporter final : public strata::log::Reader::Reporter {
 public:
  void Corruption(std::size_t /*bytes_dropped*/, const strata::Status& /*reason*/) override {}
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

std::string EncodeEdit(bool with_file) {
  strata::VersionEdit edit;
  edit.SetComparatorName("strata.BytewiseComparator");
  edit.SetLogNumber(3);
  edit.SetNextFile(8);
  edit.SetLastSequence(42);
  if (with_file) {
    std::string smallest;
    std::string largest;
    strata::AppendInternalKey(&smallest, {"a", 1, strata::ValueType::kValue});
    strata::AppendInternalKey(&largest, {"z", 2, strata::ValueType::kValue});
    edit.AddFile(0, {.number = 7, .file_size = 128, .smallest = smallest, .largest = largest});
    edit.RemoveFile(0, 6);
  }
  std::string encoded;
  edit.EncodeTo(&encoded);
  return encoded;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  strata::test::StringSource source(std::string(reinterpret_cast<const char*>(data), size));
  Reporter reporter;
  strata::log::Reader reader(&source, &reporter);
  strata::Slice record;
  std::string scratch;
  std::size_t payload_bytes = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    payload_bytes += record.size();
    if (payload_bytes > size) {
      std::abort();
    }
    strata::VersionEdit edit;
    if (edit.DecodeFrom(record).ok()) {
      std::string encoded;
      edit.EncodeTo(&encoded);
      strata::VersionEdit decoded;
      if (!decoded.DecodeFrom(encoded).ok()) {
        std::abort();
      }
    }
  }
  return 0;
}

std::vector<std::string> StrataFuzzSeeds() {
  const std::string simple = EncodeEdit(false);
  const std::string with_file = EncodeEdit(true);
  return {EncodeLog({}), EncodeLog({simple}), EncodeLog({simple, with_file}),
          EncodeLog({with_file, std::string(40000, 'x')})};
}
