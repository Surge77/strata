#include "util/event_log.hpp"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace strata {
namespace {

class CapturingLogger final : public Logger {
 public:
  void Log(LogLevel level, std::string_view message) override {
    const std::lock_guard<std::mutex> guard(mutex_);
    lines_.emplace_back(level, std::string(message));
  }
  std::vector<std::pair<LogLevel, std::string>> lines() {
    const std::lock_guard<std::mutex> guard(mutex_);
    return lines_;
  }

 private:
  std::mutex mutex_;
  std::vector<std::pair<LogLevel, std::string>> lines_;
};

TEST(FormatLogfmtTest, WritesNumbersAndPlainWordsBare) {
  EXPECT_EQ(FormatLogfmt("flush", {Field("table", 12), Field("level", "L0")}),
            "event=flush table=12 level=L0");
}

TEST(FormatLogfmtTest, QuotesTextThatWouldBreakTheLine) {
  EXPECT_EQ(FormatLogfmt("e", {Field("a", "")}), "event=e a=\"\"");
  EXPECT_EQ(FormatLogfmt("e", {Field("a", "two words")}), "event=e a=\"two words\"");
  EXPECT_EQ(FormatLogfmt("e", {Field("a", "k=v")}), "event=e a=\"k=v\"");
  EXPECT_EQ(FormatLogfmt("e", {Field("a", "say \"hi\"")}), R"(event=e a="say \"hi\"")");
  EXPECT_EQ(FormatLogfmt("e", {Field("a", "back\\slash")}), R"(event=e a="back\\slash")");
}

TEST(FormatLogfmtTest, KeepsOneEventOnOneLine) {
  const std::string line =
      FormatLogfmt("e", {Field("a", "line1\nline2\tx"), Field("b", std::string("\x01\x7f", 2))});
  EXPECT_EQ(line.find('\n'), std::string::npos);
  EXPECT_EQ(line, R"(event=e a="line1\nline2\tx" b="\x01\x7f")");
}

TEST(FormatLogfmtTest, NegativeNumbersStayNumbers) {
  const EventField field = Field("delta", -5);
  EXPECT_TRUE(field.is_number);
  EXPECT_EQ(field.value, "-5");
}

TEST(EventLogTest, WritesEachEventToTheLogger) {
  CapturingLogger logger;
  EventLog events(&logger, 8);
  events.Record(LogLevel::kWarn, "wal_record_dropped", {Field("bytes", 33)});
  const auto lines = logger.lines();
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].first, LogLevel::kWarn);
  EXPECT_EQ(lines[0].second, "event=wal_record_dropped bytes=33");
}

TEST(EventLogTest, ANullLoggerOnlyKeepsEventsInMemory) {
  EventLog events(nullptr, 8);
  events.Record(LogLevel::kInfo, "db_opened");
  EXPECT_EQ(events.RecentEvents().size(), 1U);
}

TEST(EventLogTest, NumbersEventsFromOneInOrder) {
  EventLog events(nullptr, 8);
  events.Record(LogLevel::kInfo, "a");
  events.Record(LogLevel::kInfo, "b");
  const auto recent = events.RecentEvents();
  ASSERT_EQ(recent.size(), 2U);
  EXPECT_EQ(recent[0].sequence, 1U);
  EXPECT_EQ(recent[0].name, "a");
  EXPECT_EQ(recent[1].sequence, 2U);
  EXPECT_GT(recent[1].unix_micros, 0);
}

TEST(EventLogTest, RecentEventsKeepOnlyTheNewest) {
  EventLog events(nullptr, 3);
  for (int i = 0; i < 5; ++i) {
    events.Record(LogLevel::kInfo, "e" + std::to_string(i));
  }
  const auto recent = events.RecentEvents();
  ASSERT_EQ(recent.size(), 3U);
  EXPECT_EQ(recent.front().name, "e2");
  EXPECT_EQ(recent.back().name, "e4");
}

TEST(EventLogTest, RecoveryEventsOutliveTheRing) {
  EventLog events(nullptr, 2);
  events.Record(LogLevel::kInfo, "db_opening");
  events.Record(LogLevel::kInfo, "wal_replayed");
  events.MarkOpened();
  for (int i = 0; i < 10; ++i) {
    events.Record(LogLevel::kInfo, "flush");
  }
  const auto recovery = events.RecoveryEvents();
  ASSERT_EQ(recovery.size(), 2U);
  EXPECT_EQ(recovery[0].name, "db_opening");
  EXPECT_EQ(recovery[1].name, "wal_replayed");
  EXPECT_EQ(events.RecentEvents().back().name, "flush");
}

TEST(EventLogTest, RecoveryEventsAreCapped) {
  EventLog events(nullptr, 0);
  for (std::size_t i = 0; i < EventLog::kMaxRecoveryEvents + 10; ++i) {
    events.Record(LogLevel::kWarn, "wal_record_dropped");
  }
  EXPECT_EQ(events.RecoveryEvents().size(), EventLog::kMaxRecoveryEvents);
  EXPECT_TRUE(events.RecentEvents().empty());
}

TEST(EventLogTest, SetLoggerRedirectsLaterEvents) {
  CapturingLogger first;
  CapturingLogger second;
  EventLog events(&first, 4);
  events.Record(LogLevel::kInfo, "a");
  events.SetLogger(&second);
  events.Record(LogLevel::kInfo, "b");
  EXPECT_EQ(first.lines().size(), 1U);
  ASSERT_EQ(second.lines().size(), 1U);
  EXPECT_EQ(second.lines()[0].second, "event=b");
}

}  // namespace
}  // namespace strata
