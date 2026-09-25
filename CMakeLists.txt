cmake_minimum_required(VERSION 3.20)
project(insilicell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ISC_BUILD_TESTS "Build the parity harness" ON)
if(ISC_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
if(ISC_BUILD_TESTS)
  add_subdirectory(tests/parity)
endif()
