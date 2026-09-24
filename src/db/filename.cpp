#include "db/filename.hpp"

#include <charconv>
#include <format>

namespace strata {
namespace {

std::string NumberedName(const std::string& dbname, std::uint64_t number, std::string_view suffix) {
  return std::format("{}/{:06d}.{}", dbname, number, suffix);
}

// Parses an unsigned decimal that must consume the whole of `text`. Rejects
// empty strings, signs, and overflow, so "0x1.log" or "99999999999999999999.log"
// are not mistaken for database files.
bool ParseNumber(Slice text, std::uint64_t* number) {
  if (text.empty()) {
    return false;
  }
  const char* const end = text.data() + text.size();
  const auto [where, error] = std::from_chars(text.data(), end, *number);
  return error == std::errc() && where == end;
}

}  // namespace

std::string LogFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedName(dbname, number, "sst");
}

std::string DescriptorFileName(const std::string& dbname, std::uint64_t number) {
  return std::format("{}/MANIFEST-{:06d}", dbname, number);
}

std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string TempFileName(const std::string& dbname, std::uint64_t number) {
  return NumberedName(dbname, number, "dbtmp");
}

std::string LockFileName(const std::string& dbname) {
  return dbname + "/LOCK";
}

std::string InfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG";
}

std::string OldInfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG.old";
}

bool ParseFileName(Slice filename, std::uint64_t* number, FileType* type) {
  *number = 0;
  if (filename == "CURRENT") {
    *type = FileType::kCurrentFile;
    return true;
  }
  if (filename == "LOCK") {
    *type = FileType::kLockFile;
    return true;
  }
  if (filename == "LOG" || filename == "LOG.old") {
    *type = FileType::kInfoLogFile;
    return true;
  }
  if (filename.starts_with("MANIFEST-")) {
    if (!ParseNumber(filename.substr(9), number)) {
      return false;
    }
    *type = FileType::kDescriptorFile;
    return true;
  }

  const std::size_t dot = filename.find('.');
  if (dot == Slice::npos || !ParseNumber(filename.substr(0, dot), number)) {
    return false;
  }
  const Slice suffix = filename.substr(dot + 1);
  if (suffix == "log") {
    *type = FileType::kLogFile;
  } else if (suffix == "sst") {
    *type = FileType::kTableFile;
  } else if (suffix == "dbtmp") {
    *type = FileType::kTempFile;
  } else {
    return false;
  }
  return true;
}

}  // namespace strata
