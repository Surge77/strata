add_executable(stratactl main.cpp)
target_link_libraries(stratactl PRIVATE strata strata_warnings)
# dump-sst reads the table format directly, which is internal to the library.
target_include_directories(stratactl PRIVATE ${PROJECT_SOURCE_DIR}/src)
