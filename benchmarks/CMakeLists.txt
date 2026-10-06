# Benchmarks link the engine's internals (for Histogram), so they see src/.
add_executable(negative_lookup_bench negative_lookup_bench.cpp)
target_include_directories(negative_lookup_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(negative_lookup_bench PRIVATE strata::strata strata_warnings)

add_executable(write_amp_bench write_amp_bench.cpp)
target_include_directories(write_amp_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(write_amp_bench PRIVATE strata::strata strata_warnings)

add_executable(concurrency_bench concurrency_bench.cpp)
target_link_libraries(concurrency_bench PRIVATE strata::strata strata_warnings)
