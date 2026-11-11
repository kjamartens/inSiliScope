# FFT library benchmark

This compares the forward real 2D FFT of the WideField mean-field scene (`WidefieldScene::PlaneSpectrum` →
`sim::RealFft2d`) with other FFT libraries whose licences allow use in the viewer (BSD-3), on the scene's own grid
sizes. It is not part of the main build.

```bash
git clone --depth 1 -b cpp https://github.com/mreineck/pocketfft tools/fft_bench/third_party/pocketfft
git clone --depth 1 https://github.com/marton78/pffft tools/fft_bench/third_party/pffft
cmake -S tools/fft_bench -B build/fft_bench && cmake --build build/fft_bench --config Release
ISC_THREADS=1 build/fft_bench/Release/fft_bench.exe 149
```

- `third_party/` is gitignored.
- **Always use `ISC_THREADS=1`.** `RealFft2d::Forward` runs its rows and columns in `ParallelFor`, so without it our
  FFT runs on every core while the others run on one.

## Input

- The scene's FFT grids (`SetupFft`, `FastSize(m, 8)`) are 240², 360² and 640² for a 128 / 256 / 512 px field of view
  with 100 nm pixels, `wf-upscale` 1 and `wf-kernel-um` 7.
- About 149 dye planes are transformed per image, all at full resolution. The coarse levels are never used.
- Each plane is a 0.75 n block at the origin with 30 % of its cells nonzero.

## Results (2026-11-11)

Native, MSVC `/O2 /fp:precise`, one thread, ms per plane (best of 3 batches of 149):

| n   | RealFft2d | pocketfft | pffft (SSE) |
|-----|-----------|-----------|-------------|
| 240 | 0.32      | 0.34      | – (size)    |
| 256 | 0.30      | 0.35      | 0.16        |
| 360 | 0.84      | 0.83      | – (size)    |
| 384 | 0.76      | 1.26      | 0.46        |
| 640 | 3.79      | 4.55      | 2.49        |

- **Accuracy.** Every library is within 3–6e-8 of pocketfft in double, relative to the spectrum's largest value.
- **pffft's sizes.**
  - It needs real sizes of 32 × 2^a 3^b 5^c and complex sizes of 16 × 2^a 3^b 5^c. 240 and 360 are not such sizes.
  - Using it would mean `FastSize(m, 32)`: the 256 px grid grows from 360² to 384² (+14 % area). Even so, a 384²
    pffft transform is 1.8× faster than a 360² `RealFft2d`.
  - The 2D wrapper here is simple: rows real, zero rows skipped, then a column at a time through a transpose. A
    batched column pass would gain more.
- **Licences.** pffft is FFTPACKv4 / BSD-like (Pommier, Mambro, Ayguen); pocketfft is BSD-3.
- **Rejected:**
  - KissFFT: its `kiss_fftndr` crashed in this harness, and it is a simple scalar design;
  - muFFT: power-of-two sizes only;
  - FFTW: GPL.

WASM: Emscripten 6.0.10, `-O3 -msimd128`, Node, serial; pffft on its WebAssembly SIMD backend
(`src/simd/pf_wasm_float.h`). ms per plane:

```bash
emcmake cmake -S tools/fft_bench -B build/fft_bench_wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/fft_bench_wasm
node build/fft_bench_wasm/fft_bench.js 149
```

| n   | RealFft2d | pocketfft | pffft (wasm simd) |
|-----|-----------|-----------|-------------------|
| 240 | 0.32      | 0.66      | – (size)          |
| 256 | 0.32      | 0.61      | 0.20              |
| 360 | 0.67–0.77 | 1.92      | – (size)          |
| 384 | 1.50–1.79 | 2.83      | 0.69              |
| 640 | 2.73      | 5.97      | 3.16              |

## Conclusion (2026-11-11)

**No switch.**
- In the viewer's WASM, a 384² pffft transform (the size it would need at 256 px) is no faster than our 360² one.
  At 640² (512 px) it is slower.
- Natively it would gain 1.5–1.9×, but the adapter renders frames on all cores. It would also move the last bits of
  every WideField image (the reference TIFFs, `adapter_pixel_hash`).
- `RealFft2d` stays. Its 384² case is slow (radix 3 · 2^7), so `FastSize` is right to avoid it.
