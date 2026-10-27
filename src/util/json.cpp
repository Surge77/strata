#include "util/json.hpp"

#include <cmath>
#include <cstddef>
#include <format>

namespace strata {
namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";  // U+FFFD

bool IsContinuation(unsigned char byte) {
  return (byte & 0xC0U) == 0x80U;
}

// The length of the well-formed UTF-8 sequence starting at text[pos] (RFC
// 3629: no overlong forms, no surrogates, nothing above U+10FFFF), or 0.
std::size_t Utf8SequenceLength(std::string_view text, std::size_t pos) {
  const auto lead = static_cast<unsigned char>(text[pos]);
  std::size_t length = 0;
  unsigned char low = 0x80;  // allowed range of the second byte
  unsigned char high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0) {
      low = 0xA0;  // overlong below U+0800
    } else if (lead == 0xED) {
      high = 0x9F;  // UTF-16 surrogates
    }
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    if (lead == 0xF0) {
      low = 0x90;  // overlong below U+10000
    } else if (lead == 0xF4) {
      high = 0x8F;  // above U+10FFFF
    }
  } else {
    return 0;
  }
  if (pos + length > text.size()) {
    return 0;
  }
  const auto second = static_cast<unsigned char>(text[pos + 1]);
  if (second < low || second > high) {
    return 0;
  }
  for (std::size_t i = 2; i < length; ++i) {
    if (!IsContinuation(static_cast<unsigned char>(text[pos + i]))) {
      return 0;
    }
  }
  return length;
}

}  // namespace

void AppendJsonString(std::string* out, std::string_view text) {
  out->push_back('"');
  std::size_t pos = 0;
  while (pos < text.size()) {
    const char c = text[pos];
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x80) {
      const std::size_t length = Utf8SequenceLength(text, pos);
      if (length == 0) {
        out->append(kReplacementCharacter);
        ++pos;
      } else {
        out->append(text.substr(pos, length));
        pos += length;
      }
      continue;
    }
    switch (c) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (byte < 0x20) {
          out->append(std::format("\\u{:04x}", byte));
        } else {
          out->push_back(c);
        }
    }
    ++pos;
  }
  out->push_back('"');
}

void AppendJsonNumber(std::string* out, double value) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  out->append(std::format("{}", value));
}

void AppendJsonNumber(std::string* out, std::uint64_t value) {
  out->append(std::format("{}", value));
}

void AppendJsonNumber(std::string* out, std::int64_t value) {
  out->append(std::format("{}", value));
}

}  // namespace strata
