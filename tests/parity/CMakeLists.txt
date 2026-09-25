add_executable(isc_parity parity_main.cpp)
target_link_libraries(isc_parity PRIVATE insilicell_core)
# White-box test: needs core's internal headers.
target_include_directories(isc_parity PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
if(MSVC)
  target_compile_definitions(isc_parity PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(EMSCRIPTEN)
  target_link_options(isc_parity PRIVATE -O3 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
endif()

# Golden vectors (spec/golden): bit-identical to the frozen JS reference.
find_program(ISC_NODE node NAMES node nodejs)
if(EMSCRIPTEN AND NOT ISC_NODE)
  set(ISC_NODE ${NODE_JS_EXECUTABLE})
endif()
if(ISC_NODE)
  add_test(NAME golden_vectors
    COMMAND ${ISC_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.mjs $<TARGET_FILE:isc_parity>)
endif()
