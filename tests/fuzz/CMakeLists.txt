# Fuzz targets. Each implements LLVMFuzzerTestOneInput and StrataFuzzSeeds().
#
# With STRATA_FUZZ_LIBFUZZER=ON (Clang only) a target links libFuzzer and runs
# coverage-guided. Otherwise it links fuzz_driver.cpp, a mutation driver that
# works with GCC (see ADR-0004). Either way, ctest runs a short smoke pass so a
# harness can never silently stop compiling or start crashing; the long runs
# the release gate needs are launched by hand or from CI, e.g.
#
#   ~/build/strata-asan/tests/fuzz/log_reader_fuzz --seconds 3600

option(STRATA_FUZZ_LIBFUZZER "Link fuzz targets against libFuzzer (Clang)" OFF)

function(strata_add_fuzzer name)
  if(STRATA_FUZZ_LIBFUZZER)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set(_smoke_args -runs=20000)
  else()
    add_executable(${name} ${name}.cpp fuzz_driver.cpp)
    set(_smoke_args --iterations 20000)
  endif()
  target_link_libraries(${name} PRIVATE strata strata_test_support strata_warnings)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
  if(STRATA_TEST_LAUNCHER)
    set_target_properties(${name} PROPERTIES CROSSCOMPILING_EMULATOR "${STRATA_TEST_LAUNCHER}")
  endif()
  add_test(NAME ${name}_smoke COMMAND ${name} ${_smoke_args})
  set_tests_properties(${name}_smoke PROPERTIES LABELS fuzz)
endfunction()

strata_add_fuzzer(log_reader_fuzz)
