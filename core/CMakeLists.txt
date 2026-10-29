# insiliscope core: world model, C++17, no MMDevice/GPU/OS dependencies.
option(ISC_STD_MATH "Control experiment: platform libm instead of the V8 fdlibm port" OFF)
if(ISC_STD_MATH)
  set(ISC_MATH_SRC src/jsmath_std.cpp)
else()
  set(ISC_MATH_SRC src/jsmath_fdlibm.cpp)
endif()

add_library(insiliscope_core STATIC
  src/blockstore.cpp
  src/capi.cpp
  src/cells.cpp
  src/clockmap.cpp
  src/cytomesh.cpp
  src/dyes.cpp
  src/npcs.cpp
  src/jsmath.cpp
  ${ISC_MATH_SRC}
  src/fiducials.cpp
  src/microtubules.cpp
  src/origami.cpp
  src/packing.cpp
  src/parallel.cpp
  src/params.cpp
  src/world.cpp
)
target_include_directories(insiliscope_core PUBLIC include PRIVATE src)
if(NOT EMSCRIPTEN)
  # src/parallel.cpp (serial under Emscripten without pthreads).
  find_package(Threads REQUIRED)
  target_link_libraries(insiliscope_core PUBLIC Threads::Threads)
endif()

# Floating point: no contraction into FMA, no fast-math, so native and WASM
# evaluate every expression exactly as written (and as JS does).
if(MSVC)
  # /fp:precise does not contract unless /fp:contract is given.
  target_compile_options(insiliscope_core PUBLIC /fp:precise /W3)
else()
  target_compile_options(insiliscope_core PUBLIC -ffp-contract=off -fno-fast-math -Wall)
endif()
if(EMSCRIPTEN)
  # WASM SIMD for the core too (the render code had it): f64x2 lanes are
  # IEEE-exact and nothing reassociates without fast-math, so the same bits
  # (ctest golden_vectors under Node). Needs a SIMD-capable browser (every
  # current one; Safari since 16.4), as the viewer module already did.
  target_compile_options(insiliscope_core PRIVATE -msimd128)
endif()

# WASM module: only the C ABI, for the viewer / webSMLM (and M0 size numbers).
if(EMSCRIPTEN)
  add_executable(insiliscope_wasm src/wasm_entry.cpp)
  target_link_libraries(insiliscope_wasm PRIVATE insiliscope_core)
  set(ISC_EXPORTS "_isc_abi_version,_isc_pcg4d,_isc_hash_unit,_isc_params_new,_isc_params_free,_isc_params_set,_isc_pack_window,_isc_world_new,_isc_world_free,_isc_cells_in_window,_isc_sites_in_window,_isc_density_in_window,_isc_density3d_in_window,_isc_optical_volume_in_window,_isc_world_set_dye_cache,_isc_world_prefetch,_isc_world_set_label,_isc_world_set_kinetics_history,_isc_events_in_window,_isc_continuous_in_window,_isc_world_set_clock_map,_isc_events_in_map,_isc_continuous_in_map,_isc_world_prefetch_map,_isc_world_pack_block,_isc_world_set_block,_isc_world_set_cache_dir,_isc_world_flush_cache,_isc_world_version,_isc_cell_outline,_isc_cell_mesh,_isc_cell_microtubules,_isc_cell_nucleus_rings,_isc_cell_npcs,_isc_cell_lamin,_isc_origami_in_window,_isc_origami_outline,_isc_origami_sites,_isc_origami_design_count,_isc_origami_design_id,_isc_origami_heights,_isc_fiducials_in_window,_malloc,_free")
  set(ISC_EXPORTS "${ISC_EXPORTS}" PARENT_SCOPE)   # cli/ links the viewer module with the same list
  target_link_options(insiliscope_wasm PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsiliscope
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAPF32,HEAP32,HEAPU32,stringToUTF8,lengthBytesUTF8]"
  )
  set_target_properties(insiliscope_wasm PROPERTIES OUTPUT_NAME insiliscope SUFFIX ".js")

  # The same module as one self-contained JS file (WASM inlined as base64),
  # for the viewer (web/, opened from file://, workers built from its source
  # text) and later the webSMLM block. tools/embed_web_module.mjs wraps it
  # into web/insiliscope_module.js.
  add_executable(insiliscope_web src/wasm_entry.cpp)
  target_link_libraries(insiliscope_web PRIVATE insiliscope_core)
  target_link_options(insiliscope_web PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsiliscope
    -sENVIRONMENT=web,worker
    -sSINGLE_FILE=1 -sSINGLE_FILE_BINARY_ENCODE=0
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAPF32,HEAP32,HEAPU32,stringToUTF8,lengthBytesUTF8]"
  )
  set_target_properties(insiliscope_web PROPERTIES OUTPUT_NAME insiliscope_single SUFFIX ".js")

  # (the webSMLM block's module, insiliscope_block, is in cli/CMakeLists.txt: it links the engine's drift)
endif()
