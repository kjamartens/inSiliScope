# FFT library benchmark (not part of the main build): tools/fft_bench/README.md.
#   cmake -S tools/fft_bench -B build/fft_bench && cmake --build build/fft_bench --config Release
#   build/fft_bench/Release/fft_bench.exe
# WASM: emcmake cmake -S tools/fft_bench -B build/fft_bench_wasm -DCMAKE_BUILD_TYPE=Release; node fft_bench.js
cmake_minimum_required(VERSION 3.20)
project(fft_bench C CXX)
set(CMAKE_CXX_STANDARD 17)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(SIM ${CMAKE_CURRENT_SOURCE_DIR}/../../adapter/inSiliScope/Simulation)

add_executable(fft_bench bench.cpp ${SIM}/Fft2d.cpp
  ${TP}/pffft/src/pffft.c ${TP}/pffft/src/pffft_common.c)
target_include_directories(fft_bench PRIVATE ${SIM} ${TP}/pocketfft ${TP}/pffft/include ${TP}/pffft/include/pffft ${TP}/kissfft)
target_compile_definitions(fft_bench PRIVATE kiss_fft_scalar=float _USE_MATH_DEFINES PFFFT_STATIC_DEFINE)
if(MSVC)
  # The engine's flags (core/CMakeLists.txt): /fp:precise, no contraction.
  target_compile_options(fft_bench PRIVATE /O2 /fp:precise)
elseif(EMSCRIPTEN)
  target_compile_options(fft_bench PRIVATE -O3 -msimd128 -ffp-contract=off)
  target_link_options(fft_bench PRIVATE -O3 -msimd128 -sALLOW_MEMORY_GROWTH=1)
else()
  target_compile_options(fft_bench PRIVATE -O3 -ffp-contract=off)
endif()
