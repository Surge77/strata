#include "util/json.hpp"

#include "strata/stats.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace strata {
namespace {

std::string Json(std::string_view text) {
  std::string out;
  AppendJsonString(&out, text);
  return out;
}

std::string Number(double value) {
  std::string out;
  AppendJsonNumber(&out, value);
  return out;
}

TEST(JsonStringTest, EscapesQuotesBackslashesAndControls) {
  EXPECT_EQ(Json("plain"), "\"plain\"");
  EXPECT_EQ(Json("a\"b\\c"), R"("a\"b\\c")");
  EXPECT_EQ(Json("\n\r\t"), R"("\n\r\t")");
  EXPECT_EQ(Json(std::string("\x00\x1f", 2)), R"("\u0000\u001f")");
  EXPECT_EQ(Json("\x7f"), "\"\x7f\"");  // DEL is legal unescaped in JSON
}

TEST(JsonStringTest, PassesWellFormedUtf8Through) {
  const std::string text = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";  // café € 😀
  EXPECT_EQ(Json(text), "\"" + text + "\"");
}

TEST(JsonStringTest, ReplacesEachMalformedByte) {
  const std::string replacement = "\xEF\xBF\xBD";
  EXPECT_EQ(Json("a\xFF"
                 "b"),
            "\"a" + replacement + "b\"");
  EXPECT_EQ(Json("\xC3"), "\"" + replacement + "\"");                    // truncated
  EXPECT_EQ(Json("\xC0\x80"), "\"" + replacement + replacement + "\"");  // overlong NUL
  EXPECT_EQ(Json("\xED\xA0\x80"),                                        // a surrogate
            "\"" + replacement + replacement + replacement + "\"");
  EXPECT_EQ(Json("\xF4\x90\x80\x80"),  // above U+10FFFF
            "\"" + replacement + replacement + replacement + replacement + "\"");
}

TEST(JsonNumberTest, WritesShortestRoundTripAndNullForNonFinite) {
  EXPECT_EQ(Number(0.0), "0");
  EXPECT_EQ(Number(2.5), "2.5");
  EXPECT_EQ(Number(std::numeric_limits<double>::quiet_NaN()), "null");
  EXPECT_EQ(Number(std::numeric_limits<double>::infinity()), "null");
  std::string out;
  AppendJsonNumber(&out, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(out, "18446744073709551615");
  out.clear();
  AppendJsonNumber(&out, std::int64_t{-7});
  EXPECT_EQ(out, "-7");
}

TEST(StatsToJsonTest, EmptyStatsHaveEveryTopLevelMember) {
  EXPECT_EQ(StatsToJson(Stats{}),
            R"({"unix_micros":0,"uptime_micros":0,"counters":{},"gauges":{},"latencies":{},)"
            R"("levels":[],"compaction":{"running":false,"level":0,"input_bytes":0,)"
            R"("bytes_written":0},"recovery_events":[],"recent_events":[],)"
            R"("background_error":null})");
}

TEST(StatsToJsonTest, WritesMetricsLevelsAndEvents) {
  Stats stats;
  stats.counters["get.calls"] = 3;
  stats.gauges["memtable.bytes"] = 4096;
  stats.latencies["get"] =
      LatencyStats{.count = 2,
                   .mean = 1.5,
                   .p50 = 1,
                   .p90 = 2,
                   .p99 = 2,
                   .p999 = 2,
                   .max = 2,
                   .buckets = {{.upper_micros = 1.5, .count = 1}, {.upper_micros = 2.5, .count = 1}}};
  stats.levels.push_back(LevelStats{
      .level = 1, .files = 2, .bytes = 300, .target_bytes = 1000, .compaction_score = 0.3});
  stats.compaction =
      CompactionProgress{.running = true, .level = 1, .input_bytes = 100, .bytes_written = 40};
  stats.recent_events.push_back(Event{.sequence = 9,
                                      .unix_micros = 5,
                                      .level = LogLevel::kWarn,
                                      .name = "flush",
                                      .fields = {{"table", "12", true}, {"file", "a\"b", false}}});
  stats.background_error = "IO error: disk";

  const std::string json = StatsToJson(stats);
  EXPECT_NE(json.find(R"("counters":{"get.calls":3})"), std::string::npos) << json;
  EXPECT_NE(json.find(R"("gauges":{"memtable.bytes":4096})"), std::string::npos) << json;
  EXPECT_NE(json.find(R"("get":{"count":2,"mean":1.5,"p50":1,"p90":2,"p99":2,"p999":2,"max":2,)"
                      R"("buckets":[[1.5,1],[2.5,1]]})"),
            std::string::npos)
      << json;
  EXPECT_NE(json.find(R"({"level":1,"files":2,"bytes":300,"target_bytes":1000,)"
                      R"("compaction_score":0.3})"),
            std::string::npos)
      << json;
  EXPECT_NE(json.find(R"("compaction":{"running":true,"level":1,"input_bytes":100,)"
                      R"("bytes_written":40})"),
            std::string::npos)
      << json;
  EXPECT_NE(json.find(R"({"sequence":9,"unix_micros":5,"level":"WARN","name":"flush",)"
                      R"("fields":{"table":12,"file":"a\"b"}})"),
            std::string::npos)
      << json;
  EXPECT_NE(json.find(R"("background_error":"IO error: disk")"), std::string::npos) << json;
}

}  // namespace
}  // namespace strata
