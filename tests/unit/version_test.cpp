#include "strata/version.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <string_view>
#include <vector>

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

TEST(Version, StringIsThreeDottedComponents) {
  const auto parts = Split(strata::version_string(), '.');
  ASSERT_EQ(parts.size(), 3U);
  for (const auto& part : parts) {
    EXPECT_FALSE(part.empty());
  }
}

TEST(Version, StringMatchesNumericConstants) {
  const auto parts = Split(strata::version_string(), '.');
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(ParseU32(parts[0]), strata::kVersionMajor);
  EXPECT_EQ(ParseU32(parts[1]), strata::kVersionMinor);
  EXPECT_EQ(ParseU32(parts[2]), strata::kVersionPatch);
}

}  // namespace
