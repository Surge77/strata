include(FetchContent)

# Pinned to an exact tag: an unpinned test dependency makes CI non-reproducible
# and is exactly the supply-chain surface we care about elsewhere in this repo.
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# strata_add_test(<name> SUITE <unit|integration|crash|model> SOURCES <...>)
#
# SUITE becomes a ctest label so scripts/test.sh can run one tier at a time;
# crash and model suites are slow and are excluded from the default fast loop.
function(strata_add_test name)
  cmake_parse_arguments(ARG "" "SUITE" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE strata strata_warnings GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src
                                             ${CMAKE_CURRENT_SOURCE_DIR})

  # Set before discovery so gtest_discover_tests both enumerates and runs the
  # binary through the launcher (see cmake/Sanitizers.cmake).
  if(STRATA_TEST_LAUNCHER)
    set_target_properties(${name} PROPERTIES
        CROSSCOMPILING_EMULATOR "${STRATA_TEST_LAUNCHER}")
  endif()

  gtest_discover_tests(${name}
      PROPERTIES LABELS "${ARG_SUITE}"
      DISCOVERY_TIMEOUT 30)
endfunction()

add_subdirectory(unit)
