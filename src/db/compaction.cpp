#include "db/compaction.hpp"

#include <memory>
#include <utility>

#include "db/filename.hpp"
#include "table/merger.hpp"
#include "table/table_builder.hpp"

namespace strata {
namespace {

// True if no level below `output_level` can hold `user_key`, so a tombstone
// for it has nothing left to hide.
bool IsBaseLevelForKey(const Version& version, int output_level, const Comparator* user,
                       Slice user_key) {
  for (int level = output_level + 1; level < kNumLevels; ++level) {
    for (const FileMetaData& file : version.files(level)) {
      if (user->Compare(user_key, ExtractUserKey(file.smallest)) >= 0 &&
          user->Compare(user_key, ExtractUserKey(file.largest)) <= 0) {
        return false;
      }
    }
  }
  return true;
}

// Writes the merge's output as a series of tables.
class OutputWriter {
 public:
  OutputWriter(const CompactionJobOptions& options, CompactionStats* stats)
      : options_(options), stats_(stats) {}

  OutputWriter(const OutputWriter&) = delete;
  OutputWriter& operator=(const OutputWriter&) = delete;
  OutputWriter(OutputWriter&&) = delete;
  OutputWriter& operator=(OutputWriter&&) = delete;
  ~OutputWriter() = default;

  // A table is cut only between user keys: every version of one key stays in
  // one table, so the tables of a level never share a key.
  Status Add(Slice internal_key, Slice value, Slice user_key) {
    if (builder_ != nullptr && builder_->FileSize() >= options_.max_file_size &&
        options_.comparator->user_comparator()->Compare(user_key, last_user_key_) != 0) {
      STRATA_RETURN_IF_ERROR(FinishTable());
    }
    if (builder_ == nullptr) {
      STRATA_RETURN_IF_ERROR(StartTable(internal_key));
    }
    current_.largest.assign(internal_key);
    last_user_key_.assign(user_key);
    return builder_->Add(internal_key, value);
  }

  // Finishes the last table and makes every output durable and verified.
  Status Finish() {
    if (builder_ != nullptr) {
      STRATA_RETURN_IF_ERROR(FinishTable());
    }
    if (outputs_.empty()) {
      return Status::Ok();
    }
    STRATA_RETURN_IF_ERROR(options_.env->SyncDir(options_.dbname));
    for (const FileMetaData& output : outputs_) {
      STRATA_RETURN_IF_ERROR(options_.tables->Find(output.number, output.file_size).status());
    }
    return Status::Ok();
  }

  // Deletes everything written, finished or not.
  void DeleteAll() {
    if (builder_ != nullptr) {
      builder_->Abandon();
      builder_ = nullptr;
      file_ = nullptr;
    }
    for (const std::uint64_t number : created_) {
      options_.tables->Evict(number);
      static_cast<void>(options_.env->RemoveFile(TableFileName(options_.dbname, number)));
    }
    outputs_.clear();
  }

  [[nodiscard]] std::vector<FileMetaData> TakeOutputs() { return std::move(outputs_); }

 private:
  Status StartTable(Slice first_key) {
    current_ = FileMetaData{};
    current_.number = options_.new_file_number();
    created_.push_back(current_.number);
    STRATA_ASSIGN_OR_RETURN(
        file_, options_.env->NewWritableFile(TableFileName(options_.dbname, current_.number)));
    builder_ = std::make_unique<table::TableBuilder>(
        table::TableOptions{.comparator = options_.comparator, .use_filter = options_.use_filter},
        file_.get());
    current_.smallest.assign(first_key);
    return Status::Ok();
  }

  Status FinishTable() {
    STRATA_RETURN_IF_ERROR(builder_->Finish());
    STRATA_RETURN_IF_ERROR(file_->Sync());
    STRATA_RETURN_IF_ERROR(file_->Close());
    current_.file_size = builder_->FileSize();
    stats_->bytes_written += current_.file_size;
    outputs_.push_back(std::move(current_));
    builder_ = nullptr;
    file_ = nullptr;
    return Status::Ok();
  }

  const CompactionJobOptions& options_;
  CompactionStats* const stats_;
  std::unique_ptr<WritableFile> file_;  // outlives builder_, which writes to it
  std::unique_ptr<table::TableBuilder> builder_;
  FileMetaData current_;
  std::string last_user_key_;
  std::vector<FileMetaData> outputs_;
  std::vector<std::uint64_t> created_;
};

Status Merge(const CompactionJobOptions& options, const Compaction& compaction,
             const Version& version, Iterator* input, OutputWriter* output) {
  const Comparator* user = options.comparator->user_comparator();
  std::string current_user_key;
  bool have_current_key = false;
  // Sequence of the previous (newer) entry for the current user key.
  SequenceNumber newer_sequence = kMaxSequenceNumber;

  for (input->SeekToFirst(); input->Valid(); input->Next()) {
    ParsedInternalKey key;
    if (!ParseInternalKey(input->key(), &key)) {
      return Status::Corruption("compaction input holds a malformed internal key");
    }
    if (!have_current_key || user->Compare(key.user_key, current_user_key) != 0) {
      current_user_key.assign(key.user_key);
      have_current_key = true;
      newer_sequence = kMaxSequenceNumber;
    }

    bool drop = false;
    if (newer_sequence <= options.smallest_snapshot) {
      // A newer version of this key is visible to every snapshot, so no
      // reader can ever reach this one.
      drop = true;
    } else if (key.type == ValueType::kDeletion && key.sequence <= options.smallest_snapshot &&
               IsBaseLevelForKey(version, compaction.output_level(), user, key.user_key)) {
      // Every reader sees the key as deleted, and nothing older lies below:
      // the tombstone has no more work to do. (Older versions in this merge
      // are dropped by the rule above.)
      drop = true;
    }
    newer_sequence = key.sequence;
    if (!drop) {
      STRATA_RETURN_IF_ERROR(output->Add(input->key(), input->value(), key.user_key));
    }
  }
  return input->status();
}

}  // namespace

Status RunCompaction(const CompactionJobOptions& options, const Compaction& compaction,
                     const Version& version, std::vector<FileMetaData>* outputs,
                     CompactionStats* stats) {
  std::vector<std::unique_ptr<Iterator>> children;
  for (const auto& level_inputs : compaction.inputs) {
    for (const FileMetaData& file : level_inputs) {
      STRATA_ASSIGN_OR_RETURN(auto child, options.tables->NewIterator(file.number, file.file_size));
      children.push_back(std::move(child));
      stats->bytes_read += file.file_size;
    }
  }
  const auto input = table::NewMergingIterator(options.comparator, std::move(children));

  OutputWriter output(options, stats);
  Status status = Merge(options, compaction, version, input.get(), &output);
  if (status.ok()) {
    status = output.Finish();
  }
  if (!status.ok()) {
    output.DeleteAll();
    return status;
  }
  *outputs = output.TakeOutputs();
  return Status::Ok();
}

}  // namespace strata
