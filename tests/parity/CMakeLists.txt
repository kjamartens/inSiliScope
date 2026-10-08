add_executable(isc_parity parity_main.cpp)
target_link_libraries(isc_parity PRIVATE insiliscope_core)
# White-box test: needs core's internal headers.
target_include_directories(isc_parity PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
if(MSVC)
  target_compile_definitions(isc_parity PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(EMSCRIPTEN)
  target_link_options(isc_parity PRIVATE -O3 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
endif()

# World checks (determinism, tiling, packing off, dye statistics).
add_executable(isc_world_tests world_tests.cpp)
target_link_libraries(isc_world_tests PRIVATE insiliscope_core)
target_include_directories(isc_world_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
if(EMSCRIPTEN)
  target_link_options(isc_world_tests PRIVATE -O3 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
endif()

# Per-phase core timing (not a test): isc_core_bench [seed] [windowUm].
add_executable(isc_core_bench core_bench.cpp)
target_link_libraries(isc_core_bench PRIVATE insiliscope_core)
target_include_directories(isc_core_bench PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
if(EMSCRIPTEN)
  target_link_options(isc_core_bench PRIVATE -O3 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
endif()

# Golden vectors (spec/golden): bit-identical to the frozen JS reference.
find_program(ISC_NODE node NAMES node nodejs)
if(EMSCRIPTEN AND NOT ISC_NODE)
  set(ISC_NODE ${NODE_JS_EXECUTABLE})
endif()
# world_checks: one test per section of world_tests.cpp (its Sections() list), so
# `ctest -j` runs them side by side; isc_world_tests without an argument runs all.
set(ISC_WORLD_SECTIONS basics determinism packing_off dye_statistics kinetics
  events_palm events_dnapaint events_dstorm cache_palm cache_dnapaint label_model
  threads_palm threads_dnapaint c_api block_injection density3d optical_volume edge_and_height
  kinetics_history)
foreach(sec IN LISTS ISC_WORLD_SECTIONS)
  if(EMSCRIPTEN)
    if(ISC_NODE)
      add_test(NAME world_checks.${sec} COMMAND ${ISC_NODE} $<TARGET_FILE:isc_world_tests> ${sec})
    endif()
  else()
    add_test(NAME world_checks.${sec} COMMAND isc_world_tests ${sec})
  endif()
endforeach()
if(ISC_NODE)
  add_test(NAME golden_vectors
    COMMAND ${ISC_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.mjs $<TARGET_FILE:isc_parity>)
endif()
