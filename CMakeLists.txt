cmake_minimum_required(VERSION 3.20)
project(insiliscope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-configuration generators (Ninja, Makefiles) build Release unless told
# otherwise: an unoptimised core is 10-30x slower and never what anyone wants.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link-time optimisation where the toolchain has it (MSVC /GL + /LTCG, GCC and
# Clang -flto, Emscripten's wasm-ld LTO): the core's small jsm:: wrappers and
# the per-dye/per-point helpers inline across translation units. Contraction
# into fused multiply-adds stays off through LTO (-ffp-contract=off is
# recorded per function; MSVC /fp:precise without /fp:contract never
# contracts), which ctest world_checks asserts (NoContraction). ISC_LTO=OFF
# turns it off.
option(ISC_LTO "Link-time optimisation for the core, the cli and the WASM modules" ON)
if(ISC_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT isc_ipo_ok OUTPUT isc_ipo_msg LANGUAGES CXX)
  if(isc_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
  else()
    message(STATUS "insiliscope: no link-time optimisation (${isc_ipo_msg})")
  endif()
endif()

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
