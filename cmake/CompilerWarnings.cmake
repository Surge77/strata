# An INTERFACE target carrying the warning set every strata target compiles with.
# Kept aggressive on purpose: a storage engine parses untrusted bytes, so
# sign-conversion and narrowing diagnostics are load-bearing, not noise.

add_library(strata_warnings INTERFACE)

set(_strata_gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  list(APPEND _strata_gcc_clang_warnings
       -Wmisleading-indentation
       -Wduplicated-cond
       -Wduplicated-branches
       -Wlogical-op
       -Wuseless-cast)
endif()

if(STRATA_WERROR)
  list(APPEND _strata_gcc_clang_warnings -Werror)
endif()

target_compile_options(strata_warnings INTERFACE ${_strata_gcc_clang_warnings})
