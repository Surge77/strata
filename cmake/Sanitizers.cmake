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

# ThreadSanitizer's shadow-memory layout is incompatible with the ASLR entropy
# Linux 6.6+ ships by default (vm.mmap_rnd_bits=32): the process aborts with
# "FATAL: ThreadSanitizer: unexpected memory mapping" before main() runs.
# Lowering the sysctl requires root; running the binary with randomization off
# does not, so TSan test binaries are launched through `setarch -R`.
set(STRATA_TEST_LAUNCHER "" CACHE INTERNAL "Launcher prefix for test binaries")

if(STRATA_SANITIZER STREQUAL "thread")
  find_program(STRATA_SETARCH setarch)
  if(STRATA_SETARCH)
    set(STRATA_TEST_LAUNCHER "${STRATA_SETARCH};-R" CACHE INTERNAL "")
    message(STATUS "TSan: launching tests via ${STRATA_SETARCH} -R (ASLR disabled)")
  else()
    message(WARNING "setarch not found; ThreadSanitizer tests may abort at startup")
  endif()
endif()
