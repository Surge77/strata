#include "db/version_set.hpp"

#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "db/filename.hpp"
#include "log/log_writer.hpp"
#include "support/test_dir.hpp"

namespace {

using strata::BytewiseComparator;
using strata::Env;
using strata::FileMetaData;
using strata::InternalKeyComparator;
using strata::ParsedInternalKey;
using strata::Status;
using strata::ValueType;
using strata::VersionEdit;
using strata::VersionSet;
using strata::test::TestDir;

class FailCurrentSyncDirEnv final : public strata::EnvWrapper {
 public:
  explicit FailCurrentSyncDirEnv(Env* target) : EnvWrapper(target) {}

  Status SyncDir(const std::string& /*dir*/) override {
    return Status::IoError("injected CURRENT directory sync failure");
  }
};

std::string IKey(const std::string& user_key, strata::SequenceNumber sequence) {
  std::string result;
  strata::AppendInternalKey(&result, ParsedInternalKey{user_key, sequence, ValueType::kValue});
  return result;
}

FileMetaData File(std::uint64_t number, const std::string& smallest, const std::string& largest) {
  return FileMetaData{.number = number,
                      .file_size = 100,
                      .smallest = IKey(smallest, 1),
                      .largest = IKey(largest, 1)};
}

class VersionSetTest : public ::testing::Test {
 protected:
  VersionSetTest() { EXPECT_TRUE(Env::Default()->CreateDir(dbname_).ok()); }

  std::unique_ptr<VersionSet> NewVersionSet() {
    return std::make_unique<VersionSet>(dbname_, Env::Default(), &comparator_);
  }

  // Writes a MANIFEST holding `edits` and points CURRENT at it.
  void WriteManifest(const std::vector<VersionEdit>& edits) {
    auto file = Env::Default()->NewWritableFile(strata::DescriptorFileName(dbname_, 1));
    ASSERT_TRUE(file.ok());
    strata::log::Writer writer(file->get());
    for (const VersionEdit& edit : edits) {
      std::string record;
      edit.EncodeTo(&record);
      ASSERT_TRUE(writer.AddRecord(record).ok());
    }
    ASSERT_TRUE((*file)->Close().ok());
    std::ofstream(strata::CurrentFileName(dbname_)) << "MANIFEST-000001\n";
  }

  TestDir dir_{"version_set_test"};
  const std::string dbname_ = dir_.File("db");
  const InternalKeyComparator comparator_{BytewiseComparator()};
};

TEST_F(VersionSetTest, EmptyDirectoryHasNoDatabase) {
  auto versions = NewVersionSet();
  bool existing = true;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  EXPECT_FALSE(existing);
}

TEST_F(VersionSetTest, CommittedEditsSurviveRecovery) {
  {
    auto versions = NewVersionSet();
    bool existing = false;
    ASSERT_TRUE(versions->Recover(&existing).ok());
    versions->MarkFileNumberUsed(7);
    versions->SetLastSequence(42);
    VersionEdit first;
    first.SetLogNumber(3);
    first.AddFile(0, File(5, "a", "m"));
    first.AddFile(1, File(6, "n", "z"));
    ASSERT_TRUE(versions->LogAndApply(&first).ok());
    const std::uint64_t manifest = versions->manifest_file_number();
    VersionEdit second;
    second.RemoveFile(0, 5);
    ASSERT_TRUE(versions->LogAndApply(&second).ok());
    EXPECT_EQ(versions->manifest_file_number(), manifest) << "appends to the same MANIFEST";
  }
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  EXPECT_TRUE(existing);
  EXPECT_EQ(versions->log_number(), 3U);
  EXPECT_EQ(versions->last_sequence(), 42U);
  EXPECT_TRUE(versions->current()->files(0).empty());
  ASSERT_EQ(versions->current()->files(1).size(), 1U);
  EXPECT_EQ(versions->current()->files(1)[0].number, 6U);
  EXPECT_GT(versions->NewFileNumber(), versions->manifest_file_number());
}

TEST_F(VersionSetTest, DeeperLevelsAreSortedBySmallestKey) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  versions->MarkFileNumberUsed(10);
  VersionEdit edit;
  edit.SetLogNumber(1);
  edit.AddFile(1, File(9, "p", "q"));
  edit.AddFile(1, File(8, "a", "b"));
  ASSERT_TRUE(versions->LogAndApply(&edit).ok());
  const auto& files = versions->current()->files(1);
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[0].number, 8U);
  EXPECT_EQ(files[1].number, 9U);
}

// A reader holding an old Version keeps its files alive for GC.
TEST_F(VersionSetTest, FilesOfAHeldVersionStayLive) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  versions->MarkFileNumberUsed(10);
  VersionEdit add;
  add.SetLogNumber(1);
  add.AddFile(0, File(5, "a", "b"));
  ASSERT_TRUE(versions->LogAndApply(&add).ok());

  auto held = versions->current();
  VersionEdit remove;
  remove.RemoveFile(0, 5);
  ASSERT_TRUE(versions->LogAndApply(&remove).ok());
  std::set<std::uint64_t> live;
  versions->AddLiveFiles(&live);
  EXPECT_TRUE(live.contains(5));

  held = nullptr;
  live.clear();
  versions->AddLiveFiles(&live);
  EXPECT_FALSE(live.contains(5));
}

TEST_F(VersionSetTest, CurrentNotNamingAManifestIsCorruption) {
  std::ofstream(strata::CurrentFileName(dbname_)) << "garbage\n";
  auto versions = NewVersionSet();
  bool existing = false;
  EXPECT_TRUE(versions->Recover(&existing).IsCorruption());
}

TEST_F(VersionSetTest, CurrentWithoutNewlineIsCorruption) {
  std::ofstream(strata::CurrentFileName(dbname_)) << "MANIFEST-000001";
  auto versions = NewVersionSet();
  bool existing = false;
  EXPECT_TRUE(versions->Recover(&existing).IsCorruption());
}

TEST_F(VersionSetTest, ManifestMissingRequiredFieldsIsCorruption) {
  VersionEdit edit;
  edit.SetComparatorName(BytewiseComparator()->Name());
  WriteManifest({edit});
  auto versions = NewVersionSet();
  bool existing = false;
  EXPECT_TRUE(versions->Recover(&existing).IsCorruption());
}

TEST_F(VersionSetTest, DifferentComparatorIsRejected) {
  VersionEdit edit;
  edit.SetComparatorName("some.other.Comparator");
  edit.SetLogNumber(1);
  edit.SetNextFile(2);
  edit.SetLastSequence(0);
  WriteManifest({edit});
  auto versions = NewVersionSet();
  bool existing = false;
  EXPECT_TRUE(versions->Recover(&existing).IsInvalidArgument());
}

TEST_F(VersionSetTest, DamagedManifestRecordIsCorruption) {
  VersionEdit edit;
  edit.SetLogNumber(1);
  edit.SetNextFile(2);
  edit.SetLastSequence(0);
  WriteManifest({edit, edit});
  {
    std::fstream file(strata::DescriptorFileName(dbname_, 1),
                      std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0);
    file.put('\x5a');  // breaks the first record's checksum
  }
  auto versions = NewVersionSet();
  bool existing = false;
  EXPECT_TRUE(versions->Recover(&existing).IsCorruption());
}

TEST_F(VersionSetTest, FailedCurrentDirectorySyncKeepsTheRenamedManifest) {
  FailCurrentSyncDirEnv failing(Env::Default());
  {
    VersionSet versions(dbname_, &failing, &comparator_);
    bool existing = false;
    ASSERT_TRUE(versions.Recover(&existing).ok());
    VersionEdit edit;
    edit.SetLogNumber(1);
    EXPECT_TRUE(versions.LogAndApply(&edit).IsIoError());
  }
  auto recovered = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(recovered->Recover(&existing).ok());
  EXPECT_TRUE(existing);
  EXPECT_EQ(recovered->log_number(), 1U);
}

TEST_F(VersionSetTest, OverlappingFilesInADeeperLevelAreRejected) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  VersionEdit edit;
  edit.AddFile(1, File(10, "a", "m"));
  edit.AddFile(1, File(11, "k", "z"));
  EXPECT_TRUE(versions->LogAndApply(&edit).IsCorruption());
  EXPECT_TRUE(versions->current()->files(1).empty()) << "a rejected edit changes nothing";
}

TEST_F(VersionSetTest, FilesSharingABoundaryUserKeyAreRejected) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  VersionEdit edit;
  edit.AddFile(2, File(10, "a", "m"));
  edit.AddFile(2, File(11, "m", "z"));
  EXPECT_TRUE(versions->LogAndApply(&edit).IsCorruption());
}

TEST_F(VersionSetTest, OverlappingLevelZeroFilesAreAllowed) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  VersionEdit edit;
  edit.AddFile(0, File(10, "a", "m"));
  edit.AddFile(0, File(11, "k", "z"));
  EXPECT_TRUE(versions->LogAndApply(&edit).ok());
}

TEST_F(VersionSetTest, RemovingAFileThatIsNotThereIsRejected) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  VersionEdit edit;
  edit.RemoveFile(1, 99);
  EXPECT_TRUE(versions->LogAndApply(&edit).IsCorruption());
}

TEST_F(VersionSetTest, LevelBytesSumFileSizes) {
  auto versions = NewVersionSet();
  bool existing = false;
  ASSERT_TRUE(versions->Recover(&existing).ok());
  VersionEdit edit;
  edit.AddFile(1, File(10, "a", "c"));
  edit.AddFile(1, File(11, "d", "f"));
  ASSERT_TRUE(versions->LogAndApply(&edit).ok());
  EXPECT_EQ(versions->current()->LevelBytes(1), 200U);
  EXPECT_EQ(versions->current()->LevelBytes(2), 0U);
}

}  // namespace
