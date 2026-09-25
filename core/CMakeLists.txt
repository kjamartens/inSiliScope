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
  set(ISC_EXPORTS "_isc_abi_version,_isc_pcg4d,_isc_hash_unit,_isc_params_new,_isc_params_free,_isc_params_set,_isc_pack_window,_malloc,_free")
  target_link_options(insilicell_wasm PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsilicell
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAP32,HEAPU32,stringToUTF8,lengthBytesUTF8]"
  )
  set_target_properties(insilicell_wasm PROPERTIES OUTPUT_NAME insilicell SUFFIX ".js")
endif()
