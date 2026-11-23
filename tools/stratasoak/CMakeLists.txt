add_executable(stratasoak main.cpp workload.cpp)
target_link_libraries(stratasoak PRIVATE strata strata_dashboard strata_warnings)
