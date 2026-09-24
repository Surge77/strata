#pragma once

#include "strata/env.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace strata::test {

// An Env that can simulate losing power, for testing durability claims.
//
// It writes real files through the wrapped Env and remembers, per file and per
// directory, what a POSIX filesystem is actually obliged to keep. On
// SimulatePowerLoss() it discards everything else, taking the worst case the
// standard allows:
//
//   * file contents appended after the file's last successful Sync are lost;
//   * directory changes -- creating, renaming, or removing a file -- made since
//     the last successful SyncDir of that directory are undone, newest first.
//
// Real hardware can keep *more* than this (a later write can reach the disk),
// never less. Code that is correct against the worst case is correct against
// every outcome a power loss can produce, as far as data loss is concerned.
//
// A typical crash test:
//
//   env.SetFilesystemActive(false);   // the process "dies": writes now fail
//   db.reset();                       // tear down without anything reaching disk
//   ASSERT_TRUE(env.SimulatePowerLoss().ok());
//   env.SetFilesystemActive(true);    // reboot
//   ... reopen and check that every synced write survived ...
//
// Files that existed before the FaultInjectionEnv first saw them are treated as
// fully durable.
//
// Thread safety: concurrent calls are safe when they touch *different* paths,
// which is how the engine uses files (each file has exactly one owner). Two
// threads creating, renaming, or removing the *same* path at the same time are
// not supported: the existence check and the real operation are not atomic, so
// the recorded undo could describe the wrong generation of the file.
class FaultInjectionEnv final : public EnvWrapper {
 public:
  explicit FaultInjectionEnv(Env* base);

  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override;
  Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) override;
  Status RemoveFile(const std::string& path) override;
  Status RenameFile(const std::string& from, const std::string& to) override;
  Status SyncDir(const std::string& dir) override;

  // While inactive, every call that would change the filesystem fails with
  // IoError, modelling a process that has died: nothing it attempts reaches
  // the disk. Reads still work.
  void SetFilesystemActive(bool active);

  // While set, Sync and SyncDir fail with IoError and have no durable effect.
  void SetSyncFailure(bool fail);

  // Discards everything that was not durable (see the class comment).
  Status SimulatePowerLoss();

  // Number of successful Sync / SyncDir calls, so tests can assert that a code
  // path really paid for durability rather than merely returning Ok.
  [[nodiscard]] std::uint64_t sync_count() const;
  [[nodiscard]] std::uint64_t sync_dir_count() const;

  // Called by the files this Env hands out.
  Status CheckWritable() const;
  void RecordAppend(const std::string& path, std::uint64_t bytes);
  Status RecordSync(const std::string& path);
  Status InjectedSyncFailure() const;

 private:
  // How much of a file is guaranteed to survive power loss.
  struct FileState {
    std::uint64_t size = 0;
    std::uint64_t synced_size = 0;
  };

  // A directory change not yet made durable by SyncDir, with what is needed to
  // undo it.
  struct DirOp {
    enum class Kind : std::uint8_t { kCreate, kRemove, kRename };
    Kind kind = Kind::kCreate;
    std::string path{};         // created or removed file; rename source
    std::string target{};       // rename destination
    bool had_target = false;    // rename replaced an existing file
    std::string saved_bytes{};  // durable contents of a removed or replaced file
  };

  Result<std::unique_ptr<WritableFile>> OpenTracked(const std::string& path, bool append);
  Status UndoDirOp(const DirOp& op);
  std::string DurableContents(const std::string& path);

  mutable std::mutex mutex_;
  bool active_ = true;
  bool fail_syncs_ = false;
  std::uint64_t sync_count_ = 0;
  std::uint64_t sync_dir_count_ = 0;
  std::map<std::string, FileState> files_;
  std::vector<DirOp> pending_dir_ops_;
};

}  // namespace strata::test
