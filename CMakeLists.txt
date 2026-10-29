cmake_minimum_required(VERSION 3.20)
# The release version: the VERSION file, which core/include/insiliscope/version.h must repeat (the MSBuild adapter reads
# the header).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ISC_VERSION_FILE LIMIT_COUNT 1)
string(STRIP "${ISC_VERSION_FILE}" ISC_VERSION_FILE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/core/include/insiliscope/version.h" ISC_VERSION_HEADER)
if(NOT ISC_VERSION_HEADER MATCHES "#define ISC_VERSION \"${ISC_VERSION_FILE}\"")
  message(FATAL_ERROR "core/include/insiliscope/version.h does not say ISC_VERSION \"${ISC_VERSION_FILE}\" (the VERSION file)")
endif()
project(insiliscope VERSION ${ISC_VERSION_FILE} LANGUAGES CXX)

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
# Each check would use every core: with `ctest -j 8` that is 8 x the cores. Two threads each (ISC_THREADS, read by
# core/src/parallel.cpp and Simulation/Parallel.h); a check that compares thread counts sets its own. Called at the end
# of each directory that adds tests (test properties are set where the tests are).
function(isc_cap_test_threads)
  get_property(isc_tests DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY TESTS)
  if(isc_tests)
    set_tests_properties(${isc_tests} PROPERTIES ENVIRONMENT ISC_THREADS=2)
  endif()
endfunction()

add_subdirectory(core)
add_subdirectory(cli)
if(ISC_BUILD_TESTS)
  add_subdirectory(tests/parity)
endif()
