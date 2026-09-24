#include "db/filename.hpp"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace {

using strata::FileType;

TEST(FileName, BuildsTheDocumentedNames) {
  EXPECT_EQ(strata::LogFileName("db", 7), "db/000007.log");
  EXPECT_EQ(strata::TableFileName("db", 1234567), "db/1234567.sst");
  EXPECT_EQ(strata::DescriptorFileName("db", 3), "db/MANIFEST-000003");
  EXPECT_EQ(strata::CurrentFileName("db"), "db/CURRENT");
  EXPECT_EQ(strata::TempFileName("db", 9), "db/000009.dbtmp");
  EXPECT_EQ(strata::LockFileName("db"), "db/LOCK");
  EXPECT_EQ(strata::InfoLogFileName("db"), "db/LOG");
  EXPECT_EQ(strata::OldInfoLogFileName("db"), "db/LOG.old");
}

TEST(FileName, ParsesEveryKindOfName) {
  struct Case {
    const char* name;
    std::uint64_t number;
    FileType type;
  };
  for (const Case& c :
       {Case{"000100.log", 100, FileType::kLogFile}, Case{"0.log", 0, FileType::kLogFile},
        Case{"18446744073709551615.sst", UINT64_MAX, FileType::kTableFile},
        Case{"MANIFEST-000002", 2, FileType::kDescriptorFile},
        Case{"CURRENT", 0, FileType::kCurrentFile}, Case{"LOCK", 0, FileType::kLockFile},
        Case{"LOG", 0, FileType::kInfoLogFile}, Case{"LOG.old", 0, FileType::kInfoLogFile},
        Case{"000009.dbtmp", 9, FileType::kTempFile}}) {
    std::uint64_t number = 999;
    FileType type = FileType::kLockFile;
    ASSERT_TRUE(strata::ParseFileName(c.name, &number, &type)) << c.name;
    EXPECT_EQ(number, c.number) << c.name;
    EXPECT_EQ(type, c.type) << c.name;
  }
}

// Anything that is not unambiguously one of our files must be ignored, so
// recovery and garbage collection never touch a stranger's file.
TEST(FileName, RejectsEverythingElse) {
  for (const char* name : {"",
                           "foo",
                           "foo.log",
                           "100",
                           "100.",
                           ".log",
                           "100.lo",
                           "100.logx",
                           "-1.log",
                           "+1.log",
                           "0x10.log",
                           "1 2.log",
                           "18446744073709551616.log",
                           "MANIFEST-",
                           "MANIFEST-3x",
                           "MANIFEST",
                           "CURRENTX",
                           "LOCKED",
                           "LOG.older",
                           "log",
                           "100.log.bak"}) {
    std::uint64_t number = 0;
    FileType type = FileType::kLockFile;
    EXPECT_FALSE(strata::ParseFileName(name, &number, &type)) << "'" << name << "'";
  }
}

TEST(FileName, RoundTripsGeneratedNames) {
  for (const std::uint64_t number : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{999999},
                                     std::uint64_t{1000000}, UINT64_MAX}) {
    const std::string path = strata::LogFileName("x", number);
    std::uint64_t parsed = 0;
    FileType type = FileType::kLockFile;
    ASSERT_TRUE(strata::ParseFileName(std::string_view(path).substr(2), &parsed, &type));
    EXPECT_EQ(parsed, number);
    EXPECT_EQ(type, FileType::kLogFile);
  }
}

}  // namespace
