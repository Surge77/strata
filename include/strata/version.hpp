#pragma once

#include <cstdint>
#include <string_view>

namespace strata {

// Library version. Distinct from the on-disk format versions, which are
// declared per-format (see docs/format/) and advance independently.
inline constexpr std::uint32_t kVersionMajor = 0;
inline constexpr std::uint32_t kVersionMinor = 1;
inline constexpr std::uint32_t kVersionPatch = 0;

// "major.minor.patch"
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace strata
