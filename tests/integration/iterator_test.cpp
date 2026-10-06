// Phase 10: DB::NewIterator across memtables, level 0, and deeper levels;
// snapshots and bounds; lifetime; and the gate -- scans that match the
// reference model while other threads write.

#include "strata/db.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::TestDir;
using Model = std::map<std::string, std::string>;

Model Scan(Iterator* it) {
  Model seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen[ToString(it->key())] = ToString(it->value());
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return seen;
}

std::vector<std::string> KeysBackward(Iterator* it) {
  std::vector<std::string> keys;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    keys.push_back(ToString(it->key()));
  }
  return keys;
}

class IteratorTest : public ::testing::Test {
 protected:
  void Open(Options options = {}) {
    options.path = dir_.File("db");
    auto opened = DBImpl::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(*opened);
  }
  void Reopen() {
    ASSERT_TRUE(db_->Close().ok());
    db_ = nullptr;
    Open();
  }
  void Put(const std::string& key, const std::string& value) {
    WriteBatch batch;
    batch.Put(key, value);
    ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
    model_[key] = value;
  }
  void Delete(const std::string& key) {
    WriteBatch batch;
    batch.Delete(key);
    ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
    model_.erase(key);
  }
  void ExpectMatchesModel() {
    auto it = db_->NewIterator(ReadOptions{});
    EXPECT_EQ(Scan(it.get()), model_);
    std::vector<std::string> expected;
    for (auto entry = model_.rbegin(); entry != model_.rend(); ++entry) {
      expected.push_back(entry->first);
    }
    EXPECT_EQ(KeysBackward(it.get()), expected);
  }

  TestDir dir_{"iterator"};
  std::unique_ptr<DBImpl> db_;
  Model model_;
};

TEST_F(IteratorTest, ScansTheMemtableLevelZeroAndDeeperLevelsAsOne) {
  Open();
  for (int index = 0; index < 50; ++index) {
    Put("k" + std::to_string(index * 3), "deep");
  }
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  ASSERT_TRUE(db_->CompactLevelForTest(0).ok());
  ASSERT_TRUE(db_->CompactLevelForTest(1).ok());  // now in level 2
  for (int index = 0; index < 50; ++index) {
    Put("k" + std::to_string(index * 3 + 1), "level0");
  }
  Put("k3", "overwritten in level 0");
  Delete("k6");
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  for (int index = 0; index < 50; ++index) {
    Put("k" + std::to_string(index * 3 + 2), "memtable");
  }
  Delete("k4");
  Put("k9", "overwritten in the memtable");
  ASSERT_FALSE(db_->FilesAtLevelForTest(2).empty());
  ASSERT_FALSE(db_->FilesAtLevelForTest(0).empty());
  ExpectMatchesModel();
}

TEST_F(IteratorTest, AnIteratorIsASnapshotOfTheMomentItWasCreated) {
  Open();
  Put("a", "1");
  Put("b", "1");
  const Model before = model_;
  auto it = db_->NewIterator(ReadOptions{});
  Put("a", "2");
  Delete("b");
  Put("c", "new");
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  ASSERT_TRUE(db_->CompactLevelForTest(0).ok());
  EXPECT_EQ(Scan(it.get()), before) << "later writes and a compaction must not show";
  ExpectMatchesModel();
}

TEST_F(IteratorTest, ReadsAtAnExplicitSnapshot) {
  Open();
  Put("a", "old");
  const Model before = model_;
  const Snapshot* snapshot = db_->GetSnapshot();
  Put("a", "new");
  Put("b", "new");
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  auto it = db_->NewIterator(ReadOptions{.snapshot = snapshot});
  ASSERT_TRUE(db_->ReleaseSnapshot(snapshot).ok());
  ASSERT_TRUE(db_->CompactLevelForTest(0).ok());
  EXPECT_EQ(Scan(it.get()), before) << "the iterator outlives the snapshot it was made from";
}

TEST_F(IteratorTest, BoundsLimitTheScan) {
  Open();
  for (const char* key : {"a", "b", "c", "d", "e"}) {
    Put(key, "v");
  }
  auto it = db_->NewIterator(ReadOptions{.lower_bound = "b", .upper_bound = "e"});
  EXPECT_EQ(Scan(it.get()), (Model{{"b", "v"}, {"c", "v"}, {"d", "v"}}));
  EXPECT_EQ(KeysBackward(it.get()), (std::vector<std::string>{"d", "c", "b"}));
}

TEST_F(IteratorTest, AnIteratorStaysUsableAfterTheDatabaseCloses) {
  Open();
  for (int index = 0; index < 200; ++index) {
    Put("k" + std::to_string(index), std::string(100, 'v'));
  }
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  Put("in memory", "m");
  const Model expected = model_;
  auto it = db_->NewIterator(ReadOptions{});
  ASSERT_TRUE(db_->Close().ok());
  db_ = nullptr;  // the DB object is gone; the iterator still reads its tables
  EXPECT_EQ(Scan(it.get()), expected);
}

TEST_F(IteratorTest, AClosedDatabaseGivesAnErrorIterator) {
  Open();
  ASSERT_TRUE(db_->Close().ok());
  auto it = db_->NewIterator(ReadOptions{});
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsInvalidArgument());

  DB closed;
  auto from_handle = closed.NewIterator();
  EXPECT_TRUE(from_handle->status().IsInvalidArgument());
}

TEST_F(IteratorTest, AReadErrorStopsTheScanWithAnError) {
  test::FaultInjectionEnv env(Env::Default());
  Options options;
  options.env = &env;
  options.block_cache_size = 0;
  Open(options);
  for (int index = 0; index < 300; ++index) {
    Put("k" + std::to_string(1000 + index), std::string(100, 'v'));
  }
  ASSERT_TRUE(db_->FlushMemTableForTest().ok());
  auto it = db_->NewIterator(ReadOptions{});
  env.SetReadFailure(".sst", 1);  // the first data block reads; the next fails
  int seen = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++seen;
  }
  env.SetReadFailure(std::nullopt);
  EXPECT_LT(seen, 300) << "the scan must stop at the failed block";
  EXPECT_TRUE(it->status().IsIoError()) << it->status().ToString();
  it = nullptr;
  ASSERT_TRUE(db_->Close().ok());
  db_ = nullptr;
}

TEST_F(IteratorTest, RandomOperationsMatchTheModel) {
  Options options;
  options.write_buffer_size = 8 * 1024;
  options.max_file_size = 4 * 1024;
  options.max_bytes_for_level_base = 16 * 1024;
  Open(options);
  std::mt19937 random(10);
  for (int step = 0; step < 3000; ++step) {
    const std::string key = "k" + std::to_string(random() % 200);
    const auto action = random() % 100;
    if (action < 60) {
      Put(key, std::string(random() % 120, 'x') + std::to_string(step));
    } else if (action < 90) {
      Delete(key);
    } else if (action < 95) {
      ASSERT_TRUE(db_->CompactLevelForTest(static_cast<int>(random() % 3)).ok());
    } else if (action < 97) {
      Reopen();
    } else {
      ExpectMatchesModel();
    }
  }
  db_->WaitForCompactionsForTest();
  ExpectMatchesModel();
}

// The phase gate. Writers update their own key range, and in the same atomic
// batch a per-writer counter recording how many batches they have applied. A
// scan reads the counters with the rest; everything it sees of a writer's
// range must equal that writer's model after exactly that many batches.
TEST_F(IteratorTest, ScansMatchTheModelUnderConcurrentWrites) {
  constexpr int kWriters = 4;
  constexpr int kBatchesPerWriter = 1500;
  constexpr int kKeysPerWriter = 40;
  Options options;
  options.write_buffer_size = 16 * 1024;
  Open(options);

  // models[w][n]: writer w's range after its first n batches.
  std::vector<std::vector<Model>> models(kWriters);
  std::vector<std::vector<WriteBatch>> batches(kWriters);
  for (int writer = 0; writer < kWriters; ++writer) {
    std::mt19937 random(static_cast<std::uint32_t>(100 + writer));
    Model model;
    models[static_cast<std::size_t>(writer)].push_back(model);
    for (int n = 1; n <= kBatchesPerWriter; ++n) {
      WriteBatch batch;
      for (int op = 0; op < 3; ++op) {
        const std::string key =
            "w" + std::to_string(writer) + "-" + std::to_string(random() % kKeysPerWriter);
        if (random() % 4 == 0) {
          batch.Delete(key);
          model.erase(key);
        } else {
          const std::string value = std::to_string(n) + "." + std::to_string(op);
          batch.Put(key, value);
          model[key] = value;
        }
      }
      batch.Put("counter-w" + std::to_string(writer), std::to_string(n));
      models[static_cast<std::size_t>(writer)].push_back(model);
      batches[static_cast<std::size_t>(writer)].push_back(std::move(batch));
    }
  }

  std::atomic<bool> writers_done{false};
  std::vector<std::thread> writers;
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([this, writer, &batches] {
      for (const WriteBatch& batch : batches[static_cast<std::size_t>(writer)]) {
        ASSERT_TRUE(db_->Write(WriteOptions{}, batch).ok());
      }
    });
  }

  int scans = 0;
  int mismatches = 0;
  auto check_one_scan = [&] {
    auto it = db_->NewIterator(ReadOptions{});
    const Model seen = Scan(it.get());
    for (int writer = 0; writer < kWriters; ++writer) {
      const std::string prefix = "w" + std::to_string(writer) + "-";
      const auto counter = seen.find("counter-w" + std::to_string(writer));
      const std::size_t applied = counter == seen.end() ? 0 : std::stoul(counter->second);
      Model range;
      for (const auto& [key, value] : seen) {
        if (key.starts_with(prefix)) {
          range[key] = value;
        }
      }
      if (range != models[static_cast<std::size_t>(writer)][applied]) {
        ++mismatches;
      }
    }
    ++scans;
  };
  std::thread scanner([&] {
    while (!writers_done.load()) {
      check_one_scan();
    }
  });
  for (auto& writer : writers) {
    writer.join();
  }
  writers_done = true;
  scanner.join();
  check_one_scan();  // and once at rest
  EXPECT_GT(scans, 5) << "the scanner must have overlapped the writers";
  EXPECT_EQ(mismatches, 0) << "a scan saw a state no writer ever produced";
}

}  // namespace
}  // namespace strata
