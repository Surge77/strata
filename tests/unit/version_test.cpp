#include "strata/version.hpp"

#include <charconv>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::vector<std::string_view> Split(std::string_view s, char sep) {
  std::vector<std::string_view> parts;
  for (std::size_t start = 0; start <= s.size();) {
    const std::size_t end = s.find(sep, start);
    if (end == std::string_view::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

std::uint32_t ParseU32(std::string_view s) {
  std::uint32_t value = 0;
  const auto* const last = s.data() + s.size();
  const auto result = std::from_chars(s.data(), last, value);
  EXPECT_EQ(result.ec, std::errc{});
  EXPECT_EQ(result.ptr, last);
  return value;
}

// "1.0.0" or "1.0.0-rc.1": the part before any pre-release label.
std::string_view Core() {
  const std::string_view version = strata::version_string();
  return version.substr(0, version.find('-'));
}

TEST(Version, CoreIsThreeDottedComponents) {
  const auto parts = Split(Core(), '.');
  ASSERT_EQ(parts.size(), 3U);
  for (const auto& part : parts) {
    EXPECT_FALSE(part.empty());
  }
}

TEST(Version, PreReleaseLabelMatchesTheConstant) {
  const std::string_view version = strata::version_string();
  const std::size_t dash = version.find('-');
  if (strata::kVersionPreRelease.empty()) {
    EXPECT_EQ(dash, std::string_view::npos);
  } else {
    ASSERT_NE(dash, std::string_view::npos);
    EXPECT_EQ(version.substr(dash + 1), strata::kVersionPreRelease);
  }
}

TEST(Version, StringMatchesNumericConstants) {
  const auto parts = Split(Core(), '.');
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(ParseU32(parts[0]), strata::kVersionMajor);
  EXPECT_EQ(ParseU32(parts[1]), strata::kVersionMinor);
  EXPECT_EQ(ParseU32(parts[2]), strata::kVersionPatch);
}

}  // namespace
