#include "db/version_set.hpp"

#include <algorithm>
#include <utility>

#include "db/filename.hpp"
#include "log/log_reader.hpp"

namespace strata {
namespace {

// MANIFEST replay reports: any damage other than a torn tail (which the log
// reader never reports) is fatal -- a MANIFEST with a hole could silently
// forget a table.
class ManifestReporter final : public log::Reader::Reporter {
 public:
  void Corruption(std::size_t /*bytes_dropped*/, const Status& reason) override {
    if (status.ok()) {
      status = reason;
    }
  }
  Status status;
};

}  // namespace

Result<MemTable::LookupResult> Version::Get(const LookupKey& key, std::string* value,
                                            TableCache* tables) const {
  const Comparator* user = comparator_->user_comparator();
  const Slice user_key = key.user_key();

  // Level-0 tables can overlap each other: search the newest (highest file
  // number) first, and every one whose range could contain the key.
  std::vector<const FileMetaData*> candidates;
  for (const FileMetaData& file : files_[0]) {
    if (user->Compare(user_key, ExtractUserKey(file.smallest)) >= 0 &&
        user->Compare(user_key, ExtractUserKey(file.largest)) <= 0) {
      candidates.push_back(&file);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const FileMetaData* a, const FileMetaData* b) { return a->number > b->number; });
  // Deeper levels are sorted and disjoint: at most one file each.
  for (int level = 1; level < kNumLevels; ++level) {
    const auto& files = files_[static_cast<std::size_t>(level)];
    const auto it = std::lower_bound(files.begin(), files.end(), key.internal_key(),
                                     [this](const FileMetaData& file, Slice target) {
                                       return comparator_->Compare(file.largest, target) < 0;
                                     });
    if (it != files.end() && user->Compare(user_key, ExtractUserKey(it->smallest)) >= 0) {
      candidates.push_back(&*it);
    }
  }

  for (const FileMetaData* file : candidates) {
    STRATA_ASSIGN_OR_RETURN(auto table, tables->Find(file->number, file->file_size));
    MemTable::LookupResult result = MemTable::LookupResult::kNotPresent;
    STRATA_RETURN_IF_ERROR(table->Seek(key.internal_key(), [&](Slice found_key, Slice found_value) {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(found_key, &parsed) || user->Compare(parsed.user_key, user_key) != 0) {
        return;
      }
      if (parsed.type == ValueType::kDeletion) {
        result = MemTable::LookupResult::kDeleted;
      } else {
        result = MemTable::LookupResult::kFound;
        value->assign(found_value);
      }
    }));
    if (result != MemTable::LookupResult::kNotPresent) {
      return result;
    }
  }
  return MemTable::LookupResult::kNotPresent;
}

VersionSet::VersionSet(std::string dbname, Env* env, const InternalKeyComparator* comparator)
    : dbname_(std::move(dbname)),
      env_(env),
      comparator_(comparator),
      current_(std::make_shared<const Version>(comparator)) {
  versions_.push_back(current_);
}

Status VersionSet::Recover(bool* existing) {
  *existing = false;
  const std::string current_name = CurrentFileName(dbname_);
  if (!env_->FileExists(current_name)) {
    return Status::Ok();
  }
  *existing = true;

  // CURRENT holds "MANIFEST-NNNNNN\n".
  STRATA_ASSIGN_OR_RETURN(auto current_file, env_->NewSequentialFile(current_name));
  std::string scratch(256, '\0');
  STRATA_ASSIGN_OR_RETURN(const Slice contents, current_file->Read(scratch.size(), scratch.data()));
  std::uint64_t manifest_number = 0;
  FileType type = FileType::kLockFile;
  if (contents.empty() || contents.back() != '\n' ||
      !ParseFileName(contents.substr(0, contents.size() - 1), &manifest_number, &type) ||
      type != FileType::kDescriptorFile) {
    return Status::Corruption("CURRENT does not name a MANIFEST");
  }

  const std::string manifest_name = DescriptorFileName(dbname_, manifest_number);
  STRATA_ASSIGN_OR_RETURN(auto manifest, env_->NewSequentialFile(manifest_name));
  ManifestReporter reporter;
  log::Reader reader(manifest.get(), &reporter);

  Version state(comparator_);
  bool have_log = false;
  bool have_next = false;
  bool have_sequence = false;
  Slice record;
  std::string record_scratch;
  while (reporter.status.ok() && reader.ReadRecord(&record, &record_scratch)) {
    VersionEdit edit;
    STRATA_RETURN_IF_ERROR(edit.DecodeFrom(record));
    if (edit.comparator() && *edit.comparator() != comparator_->user_comparator()->Name()) {
      return Status::InvalidArgument("database was written with comparator " + *edit.comparator());
    }
    Version next(comparator_);
    STRATA_RETURN_IF_ERROR(ApplyTo(state, edit, &next));
    state = std::move(next);
    if (edit.log_number()) {
      log_number_ = *edit.log_number();
      have_log = true;
    }
    if (edit.next_file_number()) {
      next_file_number_ = *edit.next_file_number();
      have_next = true;
    }
    if (edit.last_sequence()) {
      last_sequence_ = *edit.last_sequence();
      have_sequence = true;
    }
  }
  if (!reporter.status.ok()) {
    return Status::Corruption(manifest_name + ": " + std::string(reporter.status.message()));
  }
  if (!have_log || !have_next || !have_sequence) {
    return Status::Corruption(manifest_name + ": missing required fields");
  }
  MarkFileNumberUsed(manifest_number);
  MarkFileNumberUsed(log_number_);
  manifest_number_ = manifest_number;
  current_ = std::make_shared<const Version>(std::move(state));
  versions_.push_back(current_);
  return Status::Ok();
}

Status VersionSet::ApplyTo(const Version& base, const VersionEdit& edit, Version* result) const {
  for (int level = 0; level < kNumLevels; ++level) {
    auto& files = result->files_[static_cast<std::size_t>(level)];
    for (const FileMetaData& file : base.files(level)) {
      if (!edit.deleted_files().contains({level, file.number})) {
        files.push_back(file);
      }
    }
  }
  for (const auto& [level, file] : edit.new_files()) {
    result->files_[static_cast<std::size_t>(level)].push_back(file);
  }
  // Level 0 in file-number order; deeper levels by smallest key, so lookups
  // can binary-search them.
  auto& level0 = result->files_[0];
  std::sort(level0.begin(), level0.end(),
            [](const FileMetaData& a, const FileMetaData& b) { return a.number < b.number; });
  for (int level = 1; level < kNumLevels; ++level) {
    auto& files = result->files_[static_cast<std::size_t>(level)];
    std::sort(files.begin(), files.end(), [this](const FileMetaData& a, const FileMetaData& b) {
      return comparator_->Compare(a.smallest, b.smallest) < 0;
    });
  }
  return Status::Ok();
}

Status VersionSet::WriteSnapshot(log::Writer* writer) const {
  VersionEdit edit;
  edit.SetComparatorName(comparator_->user_comparator()->Name());
  for (int level = 0; level < kNumLevels; ++level) {
    for (const FileMetaData& file : current_->files(level)) {
      edit.AddFile(level, file);
    }
  }
  std::string record;
  edit.EncodeTo(&record);
  return writer->AddRecord(record);
}

Status VersionSet::SetCurrentFile(std::uint64_t manifest_number) {
  const std::uint64_t temp_number = NewFileNumber();
  const std::string temp = TempFileName(dbname_, temp_number);
  {
    STRATA_ASSIGN_OR_RETURN(auto file, env_->NewWritableFile(temp));
    const std::string contents = DescriptorFileName("", manifest_number).substr(1) + "\n";
    Status status = file->Append(contents);
    if (status.ok()) {
      status = file->Sync();
    }
    if (status.ok()) {
      status = file->Close();
    }
    if (!status.ok()) {
      static_cast<void>(env_->RemoveFile(temp));
      return status;
    }
  }
  if (Status renamed = env_->RenameFile(temp, CurrentFileName(dbname_)); !renamed.ok()) {
    static_cast<void>(env_->RemoveFile(temp));
    return renamed;
  }
  // Makes the rename -- and the new MANIFEST's own name -- durable.
  return env_->SyncDir(dbname_);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (!edit->log_number()) {
    edit->SetLogNumber(log_number_);
  }
  edit->SetLastSequence(last_sequence_);
  edit->SetNextFile(next_file_number_);

  auto next = std::make_shared<Version>(comparator_);
  STRATA_RETURN_IF_ERROR(ApplyTo(*current_, *edit, next.get()));

  std::uint64_t new_manifest = 0;
  if (manifest_log_ == nullptr) {
    new_manifest = NewFileNumber();
    edit->SetNextFile(next_file_number_ + 1);  // covers the CURRENT temp file too
    STRATA_ASSIGN_OR_RETURN(manifest_file_,
                            env_->NewWritableFile(DescriptorFileName(dbname_, new_manifest)));
    manifest_log_ = std::make_unique<log::Writer>(manifest_file_.get());
  }

  std::string record;
  edit->EncodeTo(&record);
  Status status = new_manifest != 0 ? WriteSnapshot(manifest_log_.get()) : Status::Ok();
  if (status.ok()) {
    status = manifest_log_->AddRecord(record);
  }
  if (status.ok()) {
    status = manifest_file_->Sync();  // the commit point
  }
  if (status.ok() && new_manifest != 0) {
    status = SetCurrentFile(new_manifest);
  }
  if (!status.ok()) {
    // The MANIFEST may now end in a partial record; drop it so the next
    // change starts a fresh one rather than appending after the damage.
    manifest_log_ = nullptr;
    manifest_file_ = nullptr;
    if (new_manifest != 0) {
      static_cast<void>(env_->RemoveFile(DescriptorFileName(dbname_, new_manifest)));
    }
    return status;
  }

  if (new_manifest != 0) {
    manifest_number_ = new_manifest;
  }
  log_number_ = *edit->log_number();
  current_ = std::move(next);
  versions_.push_back(current_);
  return Status::Ok();
}

void VersionSet::AddLiveFiles(std::set<std::uint64_t>* live) {
  std::erase_if(versions_,
                [](const std::weak_ptr<const Version>& version) { return version.expired(); });
  for (const auto& weak : versions_) {
    if (const auto version = weak.lock()) {
      for (int level = 0; level < kNumLevels; ++level) {
        for (const FileMetaData& file : version->files(level)) {
          live->insert(file.number);
        }
      }
    }
  }
}

}  // namespace strata
