# The benchmark machinery is a library so its statistics, key generators, and
# open-loop harness are unit tested like the engine (tests/unit/).
add_library(stratabench_lib STATIC
    stats.cpp
    key_generator.cpp
    open_loop.cpp
    bench_db.cpp
    workloads.cpp
    latency.cpp
    env_info.cpp
    report.cpp)
# Workloads use engine internals (Histogram, Random, DBImpl's compaction wait).
target_include_directories(stratabench_lib PUBLIC ${PROJECT_SOURCE_DIR}/tools
                                                  ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(stratabench_lib PUBLIC strata PRIVATE strata_warnings)
# Recorded in every report, so a Debug or sanitizer build is never published.
target_compile_definitions(stratabench_lib PRIVATE
    STRATA_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    STRATA_BENCH_SANITIZER="${STRATA_SANITIZER}")

add_executable(stratabench main.cpp)
target_link_libraries(stratabench PRIVATE stratabench_lib strata_warnings)
