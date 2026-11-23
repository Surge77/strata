add_executable(stratasoak main.cpp args.cpp monitor.cpp workload.cpp)
target_link_libraries(stratasoak PRIVATE strata strata_dashboard strata_warnings)
