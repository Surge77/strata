// Phase 8: crash matrix and model-based restart tests over the whole engine --
// log, flush, compaction, MANIFEST, CURRENT, and file GC.
//
// Every run is checked against a reference model: after a crash and power
// loss, the database must hold exactly the state after some prefix of the
// operations -- at least everything that was synced or flushed, at most one
// operation past the last acknowledged one (the one in flight may or may not
// have landed). Overwrites and deletes are part of the workload, so lost,
// reordered, or resurrected writes all change an observable value.

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.hpp"
#include "support/fault_injection_env.hpp"
#include "support/test_dir.hpp"

namespace strata {
namespace {

using test::FaultInjectionEnv;
using test::TestDir;

using State = std::map<std::string, std::string>;

constexpr int kKeySpace = 16;

struct Op {
  enum class Kind { kPut, kDelete, kFlush, kCompact, kAutoCompact };
  Kind kind = Kind::kPut;
  std::string key;
  std::string value;
  bool sync = false;
  int level = 0;
};

// Writes dominate; flushes and compactions are explicit operations so that a
// run's sequence of filesystem calls is the same every time, and crash point
// k is the same moment in every run.
std::vector<Op> MakeOps(std::uint32_t seed, int count) {
  std::mt19937 random(seed);
  std::vector<Op> ops;
  for (int index = 0; index < count; ++index) {
    const auto roll = random() % 100;
    Op op;
    if (roll < 55) {
      op.kind = Op::Kind::kPut;
      op.key = "k" + std::to_string(random() % kKeySpace);
      op.value = std::string(40 + random() % 120, static_cast<char>('a' + index % 26)) +
                 std::to_string(index);
      op.sync = random() % 6 == 0;
    } else if (roll < 75) {
      op.kind = Op::Kind::kDelete;
      op.key = "k" + std::to_string(random() % kKeySpace);
      op.sync = random() % 6 == 0;
    } else if (roll < 88) {
      op.kind = Op::Kind::kFlush;
    } else if (roll < 97) {
      op.kind = Op::Kind::kCompact;
      op.level = static_cast<int>(random() % 3);
    } else {
      op.kind = Op::Kind::kAutoCompact;
    }
    ops.push_back(std::move(op));
  }
  return ops;
}

// states[p] is the model after the first p operations of `ops`, from `start`.
std::vector<State> PrefixStates(const State& start, const std::vector<Op>& ops) {
  std::vector<State> states{start};
  for (const Op& op : ops) {
    State next = states.back();
    if (op.kind == Op::Kind::kPut) {
      next[op.key] = op.value;
    } else if (op.kind == Op::Kind::kDelete) {
      next.erase(op.key);
    }
    states.push_back(std::move(next));
  }
  return states;
}

Options CrashOptions(const std::string& path, Env* env) {
  Options options;
  options.path = path;
  options.env = env;
  // Big enough that only the workload's explicit flushes rotate the memtable;
  // small tables and level budgets so compactions span several levels.
  options.write_buffer_size = 1024 * 1024;
  options.max_file_size = 1024;
  options.max_bytes_for_level_base = 4 * 1024;
  return options;
}

struct RunResult {
  std::size_t acknowledged = 0;  // operations that returned Ok, in order
  std::size_t durable = 0;       // writes promised to survive power loss
  bool stopped_by_error = false;
};

// Applies `ops` until one fails. Synced writes and successful flushes raise
// the durable mark: after either, everything written so far must survive.
RunResult Apply(DBImpl* db, const std::vector<Op>& ops) {
  RunResult result;
  for (const Op& op : ops) {
    Status status;
    switch (op.kind) {
      case Op::Kind::kPut:
      case Op::Kind::kDelete: {
        WriteBatch batch;
        if (op.kind == Op::Kind::kPut) {
          batch.Put(op.key, op.value);
        } else {
          batch.Delete(op.key);
        }
        status = db->Write(WriteOptions{.sync = op.sync}, batch);
        break;
      }
      case Op::Kind::kFlush:
        status = db->FlushMemTableForTest();
        break;
      case Op::Kind::kCompact:
        status = db->CompactLevelForTest(op.level);
        break;
      case Op::Kind::kAutoCompact:
        db->PauseCompactionsForTest(false);
        db->WaitForCompactionsForTest();
        db->PauseCompactionsForTest(true);
        status = Status::Ok();  // a failure here makes the next operation fail
        break;
    }
    if (!status.ok()) {
      result.stopped_by_error = true;
      return result;
    }
    ++result.acknowledged;
    // A synced write, or a flush, makes every earlier write durable.
    // Compactions only rewrite what flushes already made durable.
    const bool is_write = op.kind == Op::Kind::kPut || op.kind == Op::Kind::kDelete;
    if ((is_write && op.sync) || op.kind == Op::Kind::kFlush) {
      result.durable = result.acknowledged;
    }
  }
  return result;
}

Result<State> ReadState(DBImpl* db) {
  State state;
  for (int index = 0; index < kKeySpace; ++index) {
    const std::string key = "k" + std::to_string(index);
    const auto value = db->Get(ReadOptions{}, key);
    if (value.ok()) {
      state[key] = *value;
    } else if (!value.status().IsNotFound()) {
      return value.status();
    }
  }
  return state;
}

// The prefix length in [low, high] whose model state matches, if any.
std::optional<std::size_t> MatchingPrefix(const State& recovered, const std::vector<State>& states,
                                          std::size_t low, std::size_t high) {
  for (std::size_t prefix = low; prefix <= high && prefix < states.size(); ++prefix) {
    if (states[prefix] == recovered) {
      return prefix;
    }
  }
  return std::nullopt;
}

std::string Describe(const State& state) {
  std::ostringstream out;
  for (const auto& [key, value] : state) {
    out << key << "=" << value.substr(value.size() > 4 ? value.size() - 4 : 0) << " ";
  }
  return out.str();
}

// Reopens after a crash and checks the recovered state against the model.
// Returns the matched prefix length, or a failure.
::testing::AssertionResult RecoverAndCheck(FaultInjectionEnv* env, const std::string& path,
                                           const std::vector<State>& states, const RunResult& run,
                                           std::size_t* prefix, std::unique_ptr<DBImpl>* reopened) {
  if (!env->SimulatePowerLoss().ok()) {
    return ::testing::AssertionFailure() << "power loss simulation failed";
  }
  env->SetFilesystemActive(true);
  auto opened = DBImpl::Open(CrashOptions(path, env));
  if (!opened.ok()) {
    return ::testing::AssertionFailure() << "reopen failed: " << opened.status().ToString();
  }
  auto db = std::move(*opened);
  db->PauseCompactionsForTest(true);
  const auto recovered = ReadState(db.get());
  if (!recovered.ok()) {
    return ::testing::AssertionFailure() << "read failed: " << recovered.status().ToString();
  }
  const std::size_t high = run.acknowledged + (run.stopped_by_error ? 1 : 0);
  const auto match = MatchingPrefix(*recovered, states, run.durable, high);
  if (!match) {
    return ::testing::AssertionFailure() << "recovered state matches no prefix in [" << run.durable
                                         << ", " << high << "]: " << Describe(*recovered);
  }
  // Rewriting every table must not uncover a damaged one or change an answer.
  for (int level = 0; level < 3; ++level) {
    if (const Status status = db->CompactLevelForTest(level); !status.ok()) {
      return ::testing::AssertionFailure()
             << "compacting level " << level << " after recovery failed: " << status.ToString();
    }
  }
  const auto compacted = ReadState(db.get());
  if (!compacted.ok() || *compacted != *recovered) {
    return ::testing::AssertionFailure() << "state changed when compacted after recovery";
  }
  *prefix = *match;
  *reopened = std::move(db);
  return ::testing::AssertionSuccess();
}

std::unique_ptr<DBImpl> OpenForWorkload(const std::string& path, Env* env) {
  auto opened = DBImpl::Open(CrashOptions(path, env));
  if (!opened.ok()) {
    return nullptr;
  }
  auto db = std::move(*opened);
  db->PauseCompactionsForTest(true);  // compactions run only when the workload says
  return db;
}

constexpr std::uint32_t kMatrixSeed = 2026;
constexpr int kMatrixOps = 90;

TEST(CrashMatrixTest, EveryCrashPointRecoversAModelPrefix) {
  const std::vector<Op> ops = MakeOps(kMatrixSeed, kMatrixOps);
  const std::vector<State> states = PrefixStates({}, ops);

  // Two clean runs must issue exactly the same filesystem operations, or
  // "crash point k" would name a different moment in each run.
  std::uint64_t total_ops = 0;
  for (int baseline = 0; baseline < 2; ++baseline) {
    TestDir dir("crash_matrix_baseline");
    FaultInjectionEnv env(Env::Default());
    auto db = OpenForWorkload(dir.File("db"), &env);
    ASSERT_NE(db, nullptr);
    const RunResult run = Apply(db.get(), ops);
    ASSERT_EQ(run.acknowledged, ops.size());
    ASSERT_GT(db->GetStats().compactions, 0U) << "the workload must exercise compaction";
    if (baseline == 0) {
      total_ops = env.mutating_ops();
    } else {
      ASSERT_EQ(env.mutating_ops(), total_ops) << "the workload is not deterministic";
    }
    ASSERT_TRUE(db->Close().ok());
  }
  std::printf("crash matrix: %llu crash points\n", static_cast<unsigned long long>(total_ops));

  for (std::uint64_t crash_at = 0; crash_at <= total_ops; ++crash_at) {
    TestDir dir("crash_matrix");
    FaultInjectionEnv env(Env::Default());
    env.SetCrashAfterOps(crash_at);
    RunResult run;
    if (auto db = OpenForWorkload(dir.File("db"), &env)) {
      run = Apply(db.get(), ops);
      static_cast<void>(db->Close());
    } else {
      run.stopped_by_error = true;
    }
    env.SetCrashAfterOps(std::nullopt);
    std::size_t prefix = 0;
    std::unique_ptr<DBImpl> reopened;
    ASSERT_TRUE(RecoverAndCheck(&env, dir.File("db"), states, run, &prefix, &reopened))
        << "crash after " << crash_at << " of " << total_ops << " filesystem operations";
    ASSERT_TRUE(reopened->Close().ok());
  }
}

// Long random histories with repeated crashes, power losses, and clean
// restarts. After each crash the model adopts whatever prefix was recovered
// and the workload carries on from where it stopped.
TEST(CrashMatrixTest, RandomCrashesAndRestartsMatchTheModel) {
  constexpr int kSeeds = 12;
  constexpr int kOpsPerSeed = 240;
  constexpr int kSegments = 6;
  for (int seed = 1; seed <= kSeeds; ++seed) {
    const std::vector<Op> ops = MakeOps(static_cast<std::uint32_t>(seed), kOpsPerSeed);
    std::mt19937 random(static_cast<std::uint32_t>(seed) * 7919U);
    TestDir dir("restart_model");
    FaultInjectionEnv env(Env::Default());
    State model;
    std::size_t cursor = 0;
    for (int segment = 0; segment < kSegments && cursor < ops.size(); ++segment) {
      const std::size_t length = std::min<std::size_t>(ops.size() - cursor, 10 + random() % 60);
      const std::vector<Op> slice(ops.begin() + static_cast<std::ptrdiff_t>(cursor),
                                  ops.begin() + static_cast<std::ptrdiff_t>(cursor + length));
      const std::vector<State> states = PrefixStates(model, slice);
      const bool crash = random() % 3 != 0;
      if (crash) {
        env.SetCrashAfterOps(random() % 400);
      }
      RunResult run;
      if (auto db = OpenForWorkload(dir.File("db"), &env)) {
        run = Apply(db.get(), slice);
        if (crash) {
          static_cast<void>(db->Close());
        } else {
          ASSERT_TRUE(db->Close().ok()) << "seed " << seed;
          run.durable = run.acknowledged;  // a clean close makes everything durable
        }
      } else {
        run.stopped_by_error = true;
      }
      env.SetCrashAfterOps(std::nullopt);
      if (!crash) {
        // Simulated power loss after a clean close must lose nothing.
        ASSERT_EQ(run.acknowledged, slice.size()) << "seed " << seed;
      }
      std::size_t prefix = 0;
      std::unique_ptr<DBImpl> reopened;
      ASSERT_TRUE(RecoverAndCheck(&env, dir.File("db"), states, run, &prefix, &reopened))
          << "seed " << seed << " segment " << segment;
      ASSERT_TRUE(reopened->Close().ok());
      model = states[prefix];
      cursor += length;
    }
  }
}

}  // namespace
}  // namespace strata
