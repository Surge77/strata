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

# Test-only helpers shared by every suite: temporary directories and the
# power-loss simulating FaultInjectionEnv.
add_library(strata_test_support STATIC
    support/fault_injection_env.cpp)
target_include_directories(strata_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(strata_test_support PUBLIC strata PRIVATE strata_warnings)

# strata_add_test(<name> SUITE <unit|integration|crash|model> SOURCES <...>)
#
# SUITE becomes a ctest label so scripts/test.sh can run one tier at a time;
# crash and model suites are slow and are excluded from the default fast loop.
function(strata_add_test name)
  cmake_parse_arguments(ARG "" "SUITE" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE strata strata_test_support strata_warnings
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src
                                             ${PROJECT_SOURCE_DIR}/tests
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
add_subdirectory(integration)
add_subdirectory(crash)
add_subdirectory(fuzz)

# The CLI end to end: each stratactl invocation is its own process, so this also
# exercises WAL recovery between commands.
if(STRATA_BUILD_TOOLS)
  # Run through the sanitizer launcher (setarch -R under TSan) like every other
  # test; the ASLR setting is inherited by the stratactl processes it starts.
  add_test(NAME stratactl_cli
           COMMAND ${STRATA_TEST_LAUNCHER} bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/stratactl_test.sh
                   $<TARGET_FILE:stratactl>)
  set_tests_properties(stratactl_cli PROPERTIES LABELS integration)

  add_test(NAME stratabench_cli
           COMMAND ${STRATA_TEST_LAUNCHER} bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/stratabench_test.sh
                   $<TARGET_FILE:stratabench>)
  set_tests_properties(stratabench_cli PROPERTIES LABELS integration)

  add_test(NAME stratasoak_cli
           COMMAND ${STRATA_TEST_LAUNCHER} bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/stratasoak_test.sh
                   $<TARGET_FILE:stratasoak>)
  set_tests_properties(stratasoak_cli PROPERTIES LABELS integration)

  # The release demo, run straight through, so it can never silently rot.
  add_test(NAME demo_script
           COMMAND ${CMAKE_COMMAND} -E env STRATA_BUILD_DIR=${CMAKE_BINARY_DIR}
                   ${STRATA_TEST_LAUNCHER} bash ${PROJECT_SOURCE_DIR}/scripts/demo.sh
                   --no-pause --dashboard_s 2)
  set_tests_properties(demo_script PROPERTIES LABELS integration)

  # The embedded copies must be the files in tools/dashboard.
  strata_add_test(dashboard_files_test SUITE unit SOURCES unit/dashboard_files_test.cpp)
  target_link_libraries(dashboard_files_test PRIVATE strata_dashboard)
  target_compile_definitions(dashboard_files_test PRIVATE
      STRATA_DASHBOARD_DIR="${PROJECT_SOURCE_DIR}/tools/dashboard")
endif()

# The dashboard's own logic, and its promise to read nothing but /stats, run
# under Node's built-in test runner (Node 18 or later). Without Node the test
# is not registered, and CMake says so.
find_program(STRATA_NODE node)
if(STRATA_NODE)
  execute_process(COMMAND ${STRATA_NODE} --version OUTPUT_VARIABLE _node_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX MATCH "^v([0-9]+)" _node_major "${_node_version}")
  if(CMAKE_MATCH_1 GREATER_EQUAL 18)
    add_test(NAME dashboard_js
             COMMAND ${STRATA_NODE} --test ${PROJECT_SOURCE_DIR}/tools/dashboard/dashboard.test.mjs)
    set_tests_properties(dashboard_js PROPERTIES LABELS unit)
  else()
    message(STATUS "Node ${_node_version} is older than 18: dashboard_js not registered")
  endif()
else()
  message(STATUS "Node not found: dashboard_js not registered")
endif()
