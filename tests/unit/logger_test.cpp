#include "strata/logger.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_dir.hpp"

namespace {

using strata::Env;
using strata::Logger;
using strata::LogLevel;
using strata::test::TestDir;

std::vector<std::string> ReadLines(const std::string& path) {
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  return lines;
}

std::unique_ptr<Logger> OpenLogger(const TestDir& dir, LogLevel min_level = LogLevel::kInfo) {
  auto logger = strata::NewFileLogger(Env::Default(), dir.File("LOG"), min_level);
  EXPECT_TRUE(logger.ok()) << logger.status().ToString();
  return std::move(logger).value();
}

TEST(Logger, NamesEveryLevel) {
  EXPECT_EQ(strata::LogLevelName(LogLevel::kDebug), "DEBUG");
  EXPECT_EQ(strata::LogLevelName(LogLevel::kInfo), "INFO");
  EXPECT_EQ(strata::LogLevelName(LogLevel::kWarn), "WARN");
  EXPECT_EQ(strata::LogLevelName(LogLevel::kError), "ERROR");
}

TEST(Logger, WritesTimestampLevelThreadAndMessage) {
  const TestDir dir("logger-format");
  {
    auto logger = OpenLogger(dir);
    strata::Log(logger.get(), LogLevel::kInfo, "recovered {} log files", 3);
  }
  const auto lines = ReadLines(dir.File("LOG"));
  ASSERT_EQ(lines.size(), 1U);
  const std::regex shape(
      R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z INFO  \[\d+\] recovered 3 log files)");
  EXPECT_TRUE(std::regex_match(lines[0], shape)) << lines[0];
}

TEST(Logger, DropsMessagesBelowTheMinimumLevel) {
  const TestDir dir("logger-level");
  {
    auto logger = OpenLogger(dir, LogLevel::kWarn);
    strata::Log(logger.get(), LogLevel::kDebug, "debug");
    strata::Log(logger.get(), LogLevel::kInfo, "info");
    strata::Log(logger.get(), LogLevel::kWarn, "warn");
    strata::Log(logger.get(), LogLevel::kError, "error");
  }
  const auto lines = ReadLines(dir.File("LOG"));
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_NE(lines[0].find("WARN  "), std::string::npos);
  EXPECT_NE(lines[1].find("ERROR "), std::string::npos);
}

TEST(Logger, AppendsToAnExistingLog) {
  const TestDir dir("logger-append");
  strata::Log(OpenLogger(dir).get(), LogLevel::kInfo, "first");
  strata::Log(OpenLogger(dir).get(), LogLevel::kInfo, "second");
  EXPECT_EQ(ReadLines(dir.File("LOG")).size(), 2U);
}

// Lines are flushed as they are written, so a reader sees them without the
// logger being closed -- which is what makes the log useful after a crash.
TEST(Logger, LinesAreVisibleBeforeTheLoggerCloses) {
  const TestDir dir("logger-flush");
  auto logger = OpenLogger(dir);
  strata::Log(logger.get(), LogLevel::kError, "visible");
  EXPECT_EQ(ReadLines(dir.File("LOG")).size(), 1U);
}

TEST(Logger, NullLoggerIsIgnored) {
  strata::Log(nullptr, LogLevel::kError, "nowhere {}", 1);
  SUCCEED();
}

TEST(Logger, ConcurrentWritersNeverInterleaveLines) {
  const TestDir dir("logger-threads");
  constexpr int kThreads = 8;
  constexpr int kLinesPerThread = 500;
  {
    auto logger = OpenLogger(dir);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&logger, thread] {
        for (int line = 0; line < kLinesPerThread; ++line) {
          strata::Log(logger.get(), LogLevel::kInfo, "writer={} line={} payload={}", thread, line,
                      std::string(64, static_cast<char>('a' + thread)));
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
  const auto lines = ReadLines(dir.File("LOG"));
  ASSERT_EQ(lines.size(), static_cast<std::size_t>(kThreads * kLinesPerThread));
  const std::regex whole(R"(.* INFO  \[\d+\] writer=\d line=\d+ payload=([a-h])\1{63})");
  for (const auto& line : lines) {
    ASSERT_TRUE(std::regex_match(line, whole)) << line;
  }
}

TEST(Logger, FailsToOpenInAMissingDirectory) {
  const TestDir dir("logger-missing");
  auto logger = strata::NewFileLogger(Env::Default(), dir.File("no/such/dir/LOG"));
  EXPECT_TRUE(logger.status().IsNotFound());
}

}  // namespace
