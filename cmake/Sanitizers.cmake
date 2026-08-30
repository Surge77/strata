# Sanitizer and coverage instrumentation, applied through one INTERFACE target.
# ASan and TSan are mutually exclusive, hence a single-valued STRATA_SANITIZER
# rather than independent booleans.

add_library(strata_sanitizers INTERFACE)

set(_strata_san_flags "")

if(STRATA_SANITIZER STREQUAL "address")
  set(_strata_san_flags -fsanitize=address -fno-omit-frame-pointer)
elseif(STRATA_SANITIZER STREQUAL "thread")
  set(_strata_san_flags -fsanitize=thread -fno-omit-frame-pointer)
elseif(STRATA_SANITIZER STREQUAL "undefined")
  set(_strata_san_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
elseif(STRATA_SANITIZER STREQUAL "address+undefined")
  set(_strata_san_flags -fsanitize=address,undefined
                        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(NOT STRATA_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown STRATA_SANITIZER: ${STRATA_SANITIZER}")
endif()

if(_strata_san_flags)
  target_compile_options(strata_sanitizers INTERFACE ${_strata_san_flags} -g)
  target_link_options(strata_sanitizers INTERFACE ${_strata_san_flags})
endif()

if(STRATA_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(strata_sanitizers INTERFACE
                           -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(strata_sanitizers INTERFACE -fprofile-instr-generate)
  else()
    target_compile_options(strata_sanitizers INTERFACE --coverage -O0 -g)
    target_link_options(strata_sanitizers INTERFACE --coverage)
  endif()
endif()
