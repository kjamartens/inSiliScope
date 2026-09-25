# insilicell core: world model, C++17, no MMDevice/GPU/OS dependencies.
option(ISC_STD_MATH "Control experiment: platform libm instead of the V8 fdlibm port" OFF)
if(ISC_STD_MATH)
  set(ISC_MATH_SRC src/jsmath_std.cpp)
else()
  set(ISC_MATH_SRC src/jsmath_fdlibm.cpp)
endif()

add_library(insilicell_core STATIC
  src/capi.cpp
  src/cells.cpp
  src/cytomesh.cpp
  src/dyes.cpp
  src/jsmath.cpp
  ${ISC_MATH_SRC}
  src/microtubules.cpp
  src/packing.cpp
  src/params.cpp
  src/world.cpp
)
target_include_directories(insilicell_core PUBLIC include PRIVATE src)

# Floating point: no contraction into FMA, no fast-math, so native and WASM
# evaluate every expression exactly as written (and as JS does).
if(MSVC)
  # /fp:precise does not contract unless /fp:contract is given.
  target_compile_options(insilicell_core PUBLIC /fp:precise /W3)
else()
  target_compile_options(insilicell_core PUBLIC -ffp-contract=off -fno-fast-math -Wall)
endif()

# WASM module: only the C ABI, for the viewer / webSMLM (and M0 size numbers).
if(EMSCRIPTEN)
  add_executable(insilicell_wasm src/wasm_entry.cpp)
  target_link_libraries(insilicell_wasm PRIVATE insilicell_core)
  set(ISC_EXPORTS "_isc_abi_version,_isc_pcg4d,_isc_hash_unit,_isc_params_new,_isc_params_free,_isc_params_set,_isc_pack_window,_isc_world_new,_isc_world_free,_isc_cells_in_window,_isc_sites_in_window,_isc_density_in_window,_isc_world_set_kinetics,_isc_events_in_window,_isc_cell_outline,_isc_cell_mesh,_isc_cell_microtubules,_malloc,_free")
  set(ISC_EXPORTS "${ISC_EXPORTS}" PARENT_SCOPE)   # cli/ links the viewer module with the same list
  target_link_options(insilicell_wasm PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsilicell
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAPF32,HEAP32,HEAPU32,stringToUTF8,lengthBytesUTF8]"
  )
  set_target_properties(insilicell_wasm PROPERTIES OUTPUT_NAME insilicell SUFFIX ".js")

  # The same module as one self-contained JS file (WASM inlined as base64),
  # for the viewer (web/, opened from file://, workers built from its source
  # text) and later the webSMLM block. tools/embed_web_module.mjs wraps it
  # into web/insilicell_module.js.
  add_executable(insilicell_web src/wasm_entry.cpp)
  target_link_libraries(insilicell_web PRIVATE insilicell_core)
  target_link_options(insilicell_web PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsilicell
    -sENVIRONMENT=web,worker
    -sSINGLE_FILE=1 -sSINGLE_FILE_BINARY_ENCODE=0
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAPF32,HEAP32,HEAPU32,stringToUTF8,lengthBytesUTF8]"
  )
  set_target_properties(insilicell_web PROPERTIES OUTPUT_NAME insilicell_single SUFFIX ".js")

  # The webSMLM block's module (M4): the same C ABI, self-contained, compiled
  # and instantiated synchronously (WASM_ASYNC_COMPILATION=0: the factory
  # not a Promise; plain script, MODULARIZE=0: the Emscripten 6 factory is
  # always async), because webSMLM builds structures
  # synchronously on the main thread. ~100 KB, far under the browsers' sync
  # compile limit.
  # tools/make_cellfield_block.mjs wraps it into dist/cellfield_block.js.
  add_executable(insilicell_block src/wasm_entry.cpp)
  target_link_libraries(insilicell_block PRIVATE insilicell_core)
  target_link_options(insilicell_block PRIVATE
    -O3
    -sMODULARIZE=0
    -sENVIRONMENT=web,worker
    -sSINGLE_FILE=1 -sSINGLE_FILE_BINARY_ENCODE=0
    -sWASM_ASYNC_COMPILATION=0
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAP32,stringToUTF8,lengthBytesUTF8]"
  )
  set_target_properties(insilicell_block PROPERTIES OUTPUT_NAME insilicell_block SUFFIX ".js")
endif()
