#pragma once

#include "strata/slice.hpp"

#include <cstdint>
#include <string>

namespace strata {

// The files a database directory can contain, and how their names encode
// what they are. Recovery lists the directory and uses ParseFileName to decide
// which files belong to the database; anything that does not parse is left
// alone, never deleted.
//
//   LOCK              held for the life of an open DB
//   LOG, LOG.old      the info log (diagnostics, not data)
//   NNNNNN.log        write-ahead log number N
//   NNNNNN.sst        SSTable number N                      (phase 4)
//   MANIFEST-NNNNNN   version edit log                      (phase 5)
//   CURRENT           names the live MANIFEST               (phase 5)
//   NNNNNN.dbtmp      temporary, e.g. a CURRENT being written (phase 5)
//
// Numbers come from one counter per database, so no two files ever share one.

enum class FileType : std::uint8_t {
  kLogFile,
  kTableFile,
  kDescriptorFile,
  kCurrentFile,
  kTempFile,
  kLockFile,
  kInfoLogFile,
};

[[nodiscard]] std::string LogFileName(const std::string& dbname, std::uint64_t number);
[[nodiscard]] std::string TableFileName(const std::string& dbname, std::uint64_t number);
[[nodiscard]] std::string DescriptorFileName(const std::string& dbname, std::uint64_t number);
[[nodiscard]] std::string CurrentFileName(const std::string& dbname);
[[nodiscard]] std::string TempFileName(const std::string& dbname, std::uint64_t number);
[[nodiscard]] std::string LockFileName(const std::string& dbname);
[[nodiscard]] std::string InfoLogFileName(const std::string& dbname);
[[nodiscard]] std::string OldInfoLogFileName(const std::string& dbname);

// Recognises a file *name* (not a path). Returns false for anything that is
// not one of the database's own files. `number` is set for numbered types and
// 0 otherwise.
[[nodiscard]] bool ParseFileName(Slice filename, std::uint64_t* number, FileType* type);

}  // namespace strata
