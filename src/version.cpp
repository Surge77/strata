#include "strata/version.hpp"

// STRATA_VERSION_* are injected by CMake from PROJECT_VERSION. The asserts below
// make the build fail if the hand-written constants in version.hpp ever drift
// from the version CMake believes it is building.
static_assert(strata::kVersionMajor == STRATA_VERSION_MAJOR);
static_assert(strata::kVersionMinor == STRATA_VERSION_MINOR);
static_assert(strata::kVersionPatch == STRATA_VERSION_PATCH);

namespace strata {

std::string_view version_string() noexcept {
  return STRATA_VERSION_STRING;
}

}  // namespace strata
