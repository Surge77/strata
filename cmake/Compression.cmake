# Block compression libraries, fetched at configure time and pinned to the
# exact commit their release tag names (see docs/journal/phase-14-compression.md
# for the dependency audit). Each can be left out; tables that need it then
# fail to open with NotSupported.
#
# They are built as static libraries in their own directories, so their
# warning flags never meet strata's -Werror, but they do get strata's
# sanitizer and coverage instrumentation: fuzzing a decompressor that ASan
# cannot see into would miss exactly the bugs it is for.

option(STRATA_WITH_SNAPPY "Build snappy block compression" ON)
option(STRATA_WITH_ZSTD   "Build zstd block compression"   ON)

include(FetchContent)

if(STRATA_WITH_ZSTD)
  FetchContent_Declare(zstd
      GIT_REPOSITORY https://github.com/facebook/zstd.git
      GIT_TAG        f8745da6ff1ad1e7bab384bd1f9d742439278e99  # v1.5.7
      GIT_SHALLOW    OFF  # a commit hash cannot be fetched shallowly by name
      SOURCE_SUBDIR  build/cmake)
  # Only the static library; no legacy (v0.1-v0.7) decoders, which would be
  # attack surface for formats strata never writes; no worker threads.
  set(ZSTD_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED        OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_STATIC        ON  CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_CONTRIB       OFF CACHE BOOL "" FORCE)
  set(ZSTD_LEGACY_SUPPORT      OFF CACHE BOOL "" FORCE)
  set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(zstd)
  target_compile_options(libzstd_static PRIVATE
      $<TARGET_PROPERTY:strata_sanitizers,INTERFACE_COMPILE_OPTIONS>)
  if(NOT STRATA_SANITIZER STREQUAL "none" OR STRATA_FUZZ_LIBFUZZER)
    # Its x86-64 Huffman decoder is assembly, which sanitizers cannot see.
    target_compile_definitions(libzstd_static PRIVATE ZSTD_DISABLE_ASM)
  endif()
endif()

if(STRATA_WITH_SNAPPY)
  FetchContent_Declare(snappy
      GIT_REPOSITORY https://github.com/google/snappy.git
      GIT_TAG        9c28114a38866f6deeaa826db918293bc28ae410  # 1.3.1
      GIT_SHALLOW    OFF)
  set(SNAPPY_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
  set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_INSTALL          OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(snappy)
  target_compile_options(snappy PRIVATE
      $<TARGET_PROPERTY:strata_sanitizers,INTERFACE_COMPILE_OPTIONS>)
  # snappy turns on -Werror under clang; its warnings are not ours to fix.
  target_compile_options(snappy PRIVATE -Wno-error)
endif()

# Linked by the engine; carries the STRATA_HAVE_* definitions its sources test.
add_library(strata_compression INTERFACE)
if(STRATA_WITH_ZSTD)
  target_link_libraries(strata_compression INTERFACE libzstd_static)
  target_compile_definitions(strata_compression INTERFACE STRATA_HAVE_ZSTD=1)
  target_include_directories(strata_compression SYSTEM INTERFACE ${zstd_SOURCE_DIR}/lib)
endif()
if(STRATA_WITH_SNAPPY)
  target_link_libraries(strata_compression INTERFACE snappy)
  target_include_directories(strata_compression SYSTEM INTERFACE
      ${snappy_SOURCE_DIR} ${snappy_BINARY_DIR})
  target_compile_definitions(strata_compression INTERFACE STRATA_HAVE_SNAPPY=1)
endif()
