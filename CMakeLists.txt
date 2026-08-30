cmake_minimum_required(VERSION 3.22)

project(strata
        VERSION 0.1.0
        DESCRIPTION "An embeddable LSM-tree key-value storage engine"
        LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not allowed. Build on ext4, e.g. -B ~/build/strata")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(STRATA_BUILD_TESTS      "Build the test suite"        ON)
option(STRATA_BUILD_TOOLS      "Build stratactl/stratabench" ON)
option(STRATA_BUILD_BENCHMARKS "Build the benchmark suite"   OFF)
option(STRATA_WERROR           "Treat warnings as errors"    ON)
option(STRATA_COVERAGE         "Instrument for coverage"     OFF)
set(STRATA_SANITIZER "none" CACHE STRING "none|address|thread|undefined|address+undefined")

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

add_subdirectory(src)

if(STRATA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(STRATA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(STRATA_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

message(STATUS "strata ${PROJECT_VERSION} | ${CMAKE_BUILD_TYPE} | "
               "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | "
               "sanitizer=${STRATA_SANITIZER} coverage=${STRATA_COVERAGE}")
