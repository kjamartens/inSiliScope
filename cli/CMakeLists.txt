# The core + the adapter's Simulation/ render code, no Micro-Manager:
#   native     insiliscope_cli (TIFF movies)
#   Emscripten insiliscope_scope_single.js: the viewer's module (core C ABI +
#              isc_scope_movie), embedded by tools/embed_web_module.mjs.
# Gaussian PSF only; on Windows the D3D11/JNI sources still compile and link
# (unused), elsewhere they are stubs.
set(ISC_SIM ${PROJECT_SOURCE_DIR}/adapter/inSiliScope/Simulation)
set(ISC_SIM_SRC
  ${ISC_SIM}/ScopeMovie.cpp
  ${ISC_SIM}/CellFieldSource.cpp
  ${ISC_SIM}/Fft2d.cpp
  ${ISC_SIM}/Illumination.cpp
  ${ISC_SIM}/WidefieldRender.cpp
  ${ISC_SIM}/GpuSimD3D11.cpp
  ${ISC_SIM}/PsfGeneratorBridge.cpp
  ${ISC_SIM}/SMLMBackground.cpp
  ${ISC_SIM}/SMLMNoise.cpp
  ${ISC_SIM}/SMLMPatterns.cpp
  ${ISC_SIM}/SMLMSimulation.cpp
  ${ISC_SIM}/SMLMStructures.cpp
  ${ISC_SIM}/SMLMZernike.cpp
)

if(EMSCRIPTEN)
  add_executable(insiliscope_scope_web scope_wasm.cpp ${ISC_SIM_SRC})
  target_include_directories(insiliscope_scope_web PRIVATE ${ISC_SIM})
  target_link_libraries(insiliscope_scope_web PRIVATE insiliscope_core)
  target_link_options(insiliscope_scope_web PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsiliscope
    -sENVIRONMENT=web,worker
    -sSINGLE_FILE=1 -sSINGLE_FILE_BINARY_ENCODE=0
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS},_isc_scope_options,_isc_scope_movie]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAPF32,HEAP32,HEAPU32,HEAPU16,HEAPU8,stringToUTF8,lengthBytesUTF8,UTF8ToString]"
  )
  set_target_properties(insiliscope_scope_web PROPERTIES OUTPUT_NAME insiliscope_scope_single SUFFIX ".js")
  return()
endif()

add_executable(insiliscope_cli insiliscope_cli.cpp ${ISC_SIM_SRC})
target_include_directories(insiliscope_cli PRIVATE ${ISC_SIM})
target_link_libraries(insiliscope_cli PRIVATE insiliscope_core)
if(WIN32)
  target_include_directories(insiliscope_cli PRIVATE ${PROJECT_SOURCE_DIR}/third_party/jni ${PROJECT_SOURCE_DIR}/third_party/jni/win32)
  target_compile_definitions(insiliscope_cli PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(insiliscope_cli PRIVATE d3d11 d3dcompiler dxgi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(insiliscope_cli PRIVATE Threads::Threads)
endif()

if(ISC_BUILD_TESTS AND NOT EMSCRIPTEN)
  add_executable(emitter_density_check emitter_density_check.cpp ${ISC_SIM_SRC})
  target_include_directories(emitter_density_check PRIVATE ${ISC_SIM})
  target_link_libraries(emitter_density_check PRIVATE insiliscope_core)
  if(WIN32)
    target_include_directories(emitter_density_check PRIVATE ${PROJECT_SOURCE_DIR}/third_party/jni ${PROJECT_SOURCE_DIR}/third_party/jni/win32)
    target_compile_definitions(emitter_density_check PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(emitter_density_check PRIVATE d3d11 d3dcompiler dxgi)
  else()
    target_link_libraries(emitter_density_check PRIVATE Threads::Threads)
  endif()
  add_test(NAME emitter_density COMMAND emitter_density_check)
  add_test(NAME cli_tiff COMMAND insiliscope_cli --out ${CMAKE_CURRENT_BINARY_DIR}/cli_test.tif --frames 20 --size 64 --x -4 --y -5)
  add_test(NAME cli_tiff_wf COMMAND insiliscope_cli --out ${CMAKE_CURRENT_BINARY_DIR}/cli_test_wf.tif --modality WideField --frames 10 --size 64 --x -4 --y -5)

  add_executable(widefield_check widefield_check.cpp ${ISC_SIM_SRC})
  target_include_directories(widefield_check PRIVATE ${ISC_SIM})
  target_link_libraries(widefield_check PRIVATE insiliscope_core)
  if(WIN32)
    target_include_directories(widefield_check PRIVATE ${PROJECT_SOURCE_DIR}/third_party/jni ${PROJECT_SOURCE_DIR}/third_party/jni/win32)
    target_compile_definitions(widefield_check PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(widefield_check PRIVATE d3d11 d3dcompiler dxgi)
  else()
    target_link_libraries(widefield_check PRIVATE Threads::Threads)
  endif()
  add_test(NAME widefield COMMAND widefield_check)
endif()
