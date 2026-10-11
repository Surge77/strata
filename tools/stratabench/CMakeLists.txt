# The benchmark machinery is a library so its statistics, key generators, and
# open-loop harness are unit tested like the engine (tests/unit/).
add_library(stratabench_lib STATIC
    stats.cpp)
# Workloads use engine internals (Histogram, Random, DBImpl's compaction wait).
target_include_directories(stratabench_lib PUBLIC ${PROJECT_SOURCE_DIR}/tools
                                                  ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(stratabench_lib PUBLIC strata PRIVATE strata_warnings)
