# Sources are listed explicitly rather than globbed so that adding a file is a
# reviewable change and CMake never silently misses one after a rebase.
add_library(strata STATIC
    db/builder.cpp
    db/compaction.cpp
    db/compaction_picker.cpp
    db/db.cpp
    db/db_impl.cpp
    db/db_impl_background.cpp
    db/db_impl_stats.cpp
    db/db_iter.cpp
    db/level_iterator.cpp
    db/dbformat.cpp
    db/filename.cpp
    db/memtable.cpp
    db/stats_json.cpp
    db/table_cache.cpp
    db/version_edit.cpp
    db/version_set.cpp
    db/write_batch.cpp
    log/log_reader.cpp
    log/log_writer.cpp
    obs/http.cpp
    obs/routes.cpp
    obs/stats_server.cpp
    table/block.cpp
    table/block_builder.cpp
    table/bloom_filter.cpp
    table/compression.cpp
    table/format.cpp
    table/merger.cpp
    table/table.cpp
    table/table_builder.cpp
    util/arena.cpp
    util/coding.cpp
    util/comparator.cpp
    util/crc32c.cpp
    util/env_posix.cpp
    util/event_log.cpp
    util/histogram.cpp
    util/json.cpp
    util/logger.cpp
    util/metrics.cpp
    util/status.cpp
    version.cpp)

add_library(strata::strata ALIAS strata)

target_include_directories(strata
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(STRATA_VERSION_PRERELEASE)
  set(_strata_version_string "${PROJECT_VERSION}-${STRATA_VERSION_PRERELEASE}")
else()
  set(_strata_version_string "${PROJECT_VERSION}")
endif()
target_compile_definitions(strata PRIVATE
    STRATA_VERSION_STRING="${_strata_version_string}"
    STRATA_VERSION_PRERELEASE="${STRATA_VERSION_PRERELEASE}"
    STRATA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    STRATA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    STRATA_VERSION_PATCH=${PROJECT_VERSION_PATCH})

target_link_libraries(strata
    PRIVATE strata_warnings strata_compression
    PUBLIC  strata_sanitizers)

set_target_properties(strata PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
