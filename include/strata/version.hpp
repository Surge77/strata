#pragma once

#include <cstdint>
#include <string_view>

namespace strata {

// Library version. Distinct from the on-disk format versions, which are
// declared per-format (see docs/format/) and advance independently.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
// A semantic-versioning pre-release label ("rc.1"), or empty for a release.
inline constexpr std::string_view kVersionPreRelease = "rc.1";

// "major.minor.patch", then "-" and the pre-release label if there is one.
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace strata
