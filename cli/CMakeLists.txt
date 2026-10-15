# The core + the adapter's Simulation/ render code, no Micro-Manager:
#   native     insiliscope_cli (TIFF movies)
#   Emscripten insiliscope_scope_single.js: the viewer's module (core C ABI +
#              isc_scope_movie), embedded by tools/embed_web_module.mjs.
# PSF: GibsonLanniZernike in C++ (ZernikePsf.cpp) or Gaussian; on Windows the D3D11/JNI sources still compile and link
# (unused), elsewhere they are stubs.
set(ISC_SIM ${PROJECT_SOURCE_DIR}/adapter/inSiliScope/Simulation)
set(ISC_SIM_SRC
  ${ISC_SIM}/ScopeMovie.cpp
  ${ISC_SIM}/BrightfieldRender.cpp
  ${ISC_SIM}/SampleSource.cpp
  ${ISC_SIM}/Drift.cpp
  ${ISC_SIM}/DyeLibrary.cpp
  ${ISC_SIM}/LightPath.cpp
  ${ISC_SIM}/Excitation.cpp
  ${ISC_SIM}/LightHistory.cpp
  ${ISC_SIM}/Spectra.cpp
  ${ISC_SIM}/Fft2d.cpp
  ${ISC_SIM}/BinnedBlinks.cpp
  ${ISC_SIM}/BlinkExpectation.cpp
  ${ISC_SIM}/Illumination.cpp
  ${ISC_SIM}/WidefieldRender.cpp
  ${ISC_SIM}/GpuSimD3D11.cpp
  ${ISC_SIM}/PsfGeneratorBridge.cpp
  ${ISC_SIM}/SMLMBackground.cpp
  ${ISC_SIM}/SMLMNoise.cpp
  ${ISC_SIM}/SMLMSimulation.cpp
  ${ISC_SIM}/SMLMZernike.cpp
  ${ISC_SIM}/ZernikePsf.cpp
  ${ISC_SIM}/SplatSse2.cpp
  ${ISC_SIM}/SplatAvx2.cpp
)
# The splat kernel's AVX2 copy (SplatKernel.h): this one file gets the AVX2
# flag on x86-64 and the dispatcher picks it at run time; everything else
# keeps the baseline instruction set. No contraction into FMA (/fp:precise
# without /fp:contract, -ffp-contract=off), so it is bit-exact with the
# baseline copy (ctest sr_render).
if(NOT EMSCRIPTEN AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64)")
  if(MSVC)
    set_source_files_properties(${ISC_SIM}/SplatAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(${ISC_SIM}/SplatAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
  endif()
endif()

if(EMSCRIPTEN)
  add_executable(insiliscope_scope_web scope_wasm.cpp ${ISC_SIM_SRC})
  target_include_directories(insiliscope_scope_web PRIVATE ${ISC_SIM})
  target_link_libraries(insiliscope_scope_web PRIVATE insiliscope_core)
  # WASM SIMD for the render code (FFT butterflies, multiply loops); the core
  # library is built without it. Vectorizing plain loops does not change
  # results (no fast-math, no contraction).
  target_compile_options(insiliscope_scope_web PRIVATE -msimd128)
  target_link_options(insiliscope_scope_web PRIVATE
    -O3
    -sMODULARIZE=1 -sEXPORT_NAME=createInsiliscope
    -sENVIRONMENT=web,worker
    -sSINGLE_FILE=1 -sSINGLE_FILE_BINARY_ENCODE=0
    # A movie's PSF stack alone is ~75 MB: start there instead of growing (and
    # copying the heap) from 16 MB in several steps.
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=96MB
    -sFILESYSTEM=0
    "-sEXPORTED_FUNCTIONS=[${ISC_EXPORTS},_isc_timing,_isc_scope_options,_isc_scope_movie,_isc_scope_psf_preview,_isc_wf_begin,_isc_wf_scenes,_isc_wf_select,_isc_wf_job,_isc_wf_kernel,_isc_wf_plane,_isc_wf_plane_values,_isc_wf_channel,_isc_wf_set_images,_isc_wf_cpu_images,_isc_wf_movie,_isc_wf_end,_isc_bf_begin,_isc_bf_begin_phase,_isc_bf_info,_isc_bf_phase,_isc_bf_atten,_isc_bf_source_image,_isc_bf_set_sources,_isc_bf_movie,_isc_bf_end]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPF64,HEAPF32,HEAP32,HEAPU32,HEAPU16,HEAPU8,stringToUTF8,lengthBytesUTF8,UTF8ToString]"
  )
  set_target_properties(insiliscope_scope_web PROPERTIES OUTPUT_NAME insiliscope_scope_single SUFFIX ".js")
  return()
endif()

# The render engine, compiled once (2026-10-13): the cli and every check link this library instead of compiling
# ${ISC_SIM_SRC} each (a change in ScopeMovie.cpp cost nine compiles, 254 s). Same sources, flags, defines and include
# paths as before, so the same code.
add_library(insiliscope_sim STATIC ${ISC_SIM_SRC})
target_include_directories(insiliscope_sim PUBLIC ${ISC_SIM})
target_link_libraries(insiliscope_sim PUBLIC insiliscope_core)
if(WIN32)
  target_include_directories(insiliscope_sim PUBLIC ${PROJECT_SOURCE_DIR}/third_party/jni ${PROJECT_SOURCE_DIR}/third_party/jni/win32)
  target_compile_definitions(insiliscope_sim PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(insiliscope_sim PUBLIC d3d11 d3dcompiler dxgi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(insiliscope_sim PUBLIC Threads::Threads)
endif()

add_executable(insiliscope_cli insiliscope_cli.cpp scope_probes.cpp)
target_link_libraries(insiliscope_cli PRIVATE insiliscope_sim)

if(ISC_BUILD_TESTS AND NOT EMSCRIPTEN)
  add_test(NAME cli_tiff COMMAND insiliscope_cli --out ${CMAKE_CURRENT_BINARY_DIR}/cli_test.tif --frames 20 --size 64 --x -4 --y -5)
  add_test(NAME cli_geometry COMMAND insiliscope_cli --geometry-json ${CMAKE_CURRENT_BINARY_DIR}/cli_geometry.json --geometry-um 30 --geometry-detail 1 --x 5 --y -3)
  add_test(NAME cli_tiff_bf COMMAND insiliscope_cli --out ${CMAKE_CURRENT_BINARY_DIR}/cli_test_bf.tif --modality BrightField --frames 3 --size 64 --x -4 --y -5 --bf-quality 2)
  add_test(NAME cli_tiff_wf COMMAND insiliscope_cli --out ${CMAKE_CURRENT_BINARY_DIR}/cli_test_wf.tif --mt-mode WideField --frames 10 --size 64 --x -4 --y -5)
  # The DNA-origami nanorulers (specimen 1): a DNA-PAINT movie and the molecules' geometry with every strand.
  add_test(NAME cli_origami COMMAND insiliscope_cli --specimen Nanorulers --out ${CMAKE_CURRENT_BINARY_DIR}/cli_test_origami.tif
           --frames 20 --size 48 --ruler-imager-nm 20)
  add_test(NAME cli_origami_geometry COMMAND insiliscope_cli --specimen Nanorulers --geometry-json
           ${CMAKE_CURRENT_BINARY_DIR}/cli_origami.json --geometry-um 10 --geometry-detail 1)

  add_executable(sr_render_check sr_render_check.cpp)
  target_link_libraries(sr_render_check PRIVATE insiliscope_sim)
  add_test(NAME sr_render COMMAND sr_render_check)

  add_executable(widefield_check widefield_check.cpp)
  target_link_libraries(widefield_check PRIVATE insiliscope_sim)
  add_test(NAME widefield COMMAND widefield_check)

  # The excitation geometry (epi / HILO / TIRF): the physics against quadrature and Axelrod 1981, the local kinetics.
  add_executable(excitation_check excitation_check.cpp)
  target_link_libraries(excitation_check PRIVATE insiliscope_sim)
  add_test(NAME excitation COMMAND excitation_check)

  # The exact light history (LightHistory.h): its folds and maps against independent sums, the core reading them.
  add_executable(history_check history_check.cpp)
  target_link_libraries(history_check PRIVATE insiliscope_sim)
  add_test(NAME history_check COMMAND history_check)

  add_executable(drift_check drift_check.cpp)
  target_link_libraries(drift_check PRIVATE insiliscope_sim)
  add_test(NAME drift COMMAND drift_check)

  # The blink render regimes: expected ON time = the core's blinks, binned = splat, mean-field movie totals.
  add_executable(blink_regimes_check blink_regimes_check.cpp)
  target_link_libraries(blink_regimes_check PRIVATE insiliscope_sim)
  add_test(NAME blink_regimes COMMAND blink_regimes_check)

  # The cli's diagnostic outputs (scope_probes.h: the docs' physics figures) show what a movie renders.
  add_executable(probes_check probes_check.cpp scope_probes.cpp)
  target_link_libraries(probes_check PRIVATE insiliscope_sim)
  add_test(NAME cli_probes COMMAND probes_check WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  # --serve (many commands in one process, the figure builder's way) = separate runs, byte for byte.
  add_test(NAME cli_serve COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:insiliscope_cli>
           -DDIR=${CMAKE_CURRENT_BINARY_DIR}/serve_check -P ${CMAKE_CURRENT_SOURCE_DIR}/serve_check.cmake)

  # BrightField: weak-phase-grating theory, empty field, determinism (spec/BRIGHTFIELD.md).
  add_executable(brightfield_check brightfield_check.cpp)
  target_link_libraries(brightfield_check PRIVATE insiliscope_sim)
  add_test(NAME brightfield COMMAND brightfield_check)

  # The C++ GibsonLanniZernike PSF against webSMLM's JS (tests/psf/zernike_ref.bin).
  add_executable(zernike_psf_check zernike_psf_check.cpp)
  target_link_libraries(zernike_psf_check PRIVATE insiliscope_sim)
  add_test(NAME zernike_psf COMMAND zernike_psf_check ${PROJECT_SOURCE_DIR}/tests/psf/zernike_ref.bin)
  if(WIN32)
    # The WideField GPU host on Direct3D 11 (Linux: tools/wine_wf_gpu_check.sh).
    add_executable(wf_gpu_d3d11_check ${PROJECT_SOURCE_DIR}/tests/d3d11/wf_gpu_d3d11_check.cpp
                   ${ISC_SIM}/WidefieldGpuD3D11.cpp)
    target_link_libraries(wf_gpu_d3d11_check PRIVATE insiliscope_sim)
    add_test(NAME wf_gpu_d3d11 COMMAND wf_gpu_d3d11_check)
  endif()
endif()
