#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

// Appends `text` as a JSON string literal. The output is always valid UTF-8
// JSON whatever the input bytes: '"', '\', and control characters are
// escaped, and any byte that is not part of a well-formed UTF-8 sequence
// becomes U+FFFD, because engine text (file names, error messages) is not
// guaranteed to be UTF-8 and one bad byte must not make a whole response
// unparseable.
void AppendJsonString(std::string* out, std::string_view text);

// Appends a JSON number: the shortest decimal that reads back as `value`.
// JSON has no NaN or infinity, so those are written as null.
void AppendJsonNumber(std::string* out, double value);
void AppendJsonNumber(std::string* out, std::uint64_t value);
void AppendJsonNumber(std::string* out, std::int64_t value);

}  // namespace strata
