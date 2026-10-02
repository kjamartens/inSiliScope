cmake_minimum_required(VERSION 3.20)
project(insiliscope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ISC_BUILD_TESTS "Build the parity harness" ON)
if(ISC_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(cli)
if(ISC_BUILD_TESTS)
  add_subdirectory(tests/parity)
endif()

# `cmake --build --preset lab` (or `--target lab`): the lab's dev server (web/lab/README.md), opening web/lab.html
# (the viewer on the JS reference) in the default browser; runs until Ctrl+C. Builds nothing.
find_program(ISC_NODE node)
if(ISC_NODE)
  add_custom_target(lab
    COMMAND "${ISC_NODE}" web/lab/serve.mjs --open
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "lab: http://localhost:8123/web/lab.html (Ctrl+C stops it)")
endif()
