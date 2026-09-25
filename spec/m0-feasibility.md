# M0 feasibility spike: single C++ core, native and WASM (option B)

**Verdict: PASS, go for option B.** Across 50 seeds (35 default parameters, 15 in three stressed
parameter sets), RNG and packing are **bit-identical** in JS (the prototype), native MSVC and
WASM (Emscripten). The core WASM module is **12.8 KB gzip** so far.

Date: 2026-09-25. Reference: `C:\GitHub\websmlm\cell_field_sim\index.html` run under Node
v24.18.0 (V8 13.6.233.17). Native: MSVC 2022 x64 Release, `/fp:precise`. WASM: Emscripten 6.0.10,
`-O3`, run under Node.

## What was ported

`core/`: `pcg4d`, `hashUnit`, `hashStream` (`rng.h`); `rawCandidate`, `cellRadiusAt`,
`envelopNucleus`, `cytoSlopeRunout`, `cytoDomeReach` (`cells.cpp`); `interactionChunks`,
`buildCandidateMap`, the radius LUT, `boundaryClearance`, `directionalOverlap`, `relax`, `prune`,
`packMap` (`packing.cpp`); C ABI `isc_pack_window` (`include/insiliscope/insiliscope.h`).

## Findings that shape the rest of the port

1. **Math must be V8's, not the platform's.** Packing is chaotic: with MSVC's `std::sin/cos/atan2`
   (control build `ISC_STD_MATH=ON`) raw candidates differ by only ~7e-15 µm, yet 4 of 50 windows
   end with a *different set of surviving cells* and the other 46 drift. So `core/` carries V8's
   fdlibm (`jsmath_fdlibm.cpp`, generated verbatim from V8 `src/base/ieee754.cc` by
   `tools/gen_jsmath.py`) plus V8's 2-argument `Math.hypot`. With that, sin/cos/atan/atan2/exp/log/
   hypot are bit-exact against Node on 40 000 inputs each, native and WASM. This goes beyond the
   brief's "own sin/cos in decision paths": every transcendental the JS uses goes through `jsm::`.
2. **`Math.pow` cannot be matched.** V8 defaults to `--use-std-math-pow`, so JS `Math.pow` is the
   host platform's `std::pow`. JS itself differs between OSes. The prototype uses it only in the
   cytoplasm mesh ring spacing (M2). Expect "near", not "equal", there unless the JS changes to
   `Math.exp(b*Math.log(a))` or a fixed polynomial.
3. **The prototype in a browser is not one reference.** Node's V8 uses fdlibm for sin/cos. Other
   engines, and V8 builds with `V8_USE_LIBM_TRIG_FUNCTIONS`, may not. The golden vectors are
   therefore defined as "the prototype under Node". After M4 this stops mattering: WASM carries its
   own math, so every browser produces the same field.
4. **Float32 caches are part of the algorithm.** The JS radius LUT and collision outline are
   `Float32Array`s. The C++ stores them as `float` too; using double there would change layouts.
5. A latent JS edge case is reproduced on purpose: `radiusFromLUT` reads `lut[128]` (NaN) when a
   tiny negative angle rounds up to exactly 2π.
6. Speed, 50 windows of 14×14 chunks: JS 105 s, native 3.9 s, WASM 4.0 s in this harness. The
   JS runs inside a `vm` context, so treat the ratio as indicative only. WASM runs at native speed.

## Reproduce

```sh
cmake --preset msvc && cmake --build --preset msvc
source ~/emsdk/emsdk_env.sh && cmake --preset wasm && cmake --build --preset wasm
node tests/parity/run.mjs            # -> build/parity/report.md, exit 0 = PASS
node tests/parity/wasm_abi_smoke.mjs # the C ABI from JS, as the viewer/webSMLM will call it
```

## Generated report (`node tests/parity/run.mjs`, with the `ISC_STD_MATH` control build added)

## Primitives

| check | native | wasm | native_stdmath |
|---|---|---|---|
| pcg4d / hashUnit / hashStream | bit-exact (252 addr + 4 streams) | bit-exact (252 addr + 4 streams) | bit-exact (252 addr + 4 streams) |
| Math.sin vs core jsm::sin | bit-exact (40000) | bit-exact (40000) | 972 / 40000 differ |
| Math.cos vs core jsm::cos | bit-exact (40000) | bit-exact (40000) | 925 / 40000 differ |
| Math.atan2 vs core jsm::atan2 | bit-exact (40000) | bit-exact (40000) | 7195 / 40000 differ |
| Math.hypot vs core jsm::hypot | bit-exact (40000) | bit-exact (40000) | bit-exact (40000) |
| Math.atan vs core jsm::atan | bit-exact (40000) | bit-exact (40000) | 24 / 40000 differ |
| Math.exp vs core jsm::exp | bit-exact (40000) | bit-exact (40000) | 3586 / 40000 differ |
| Math.log vs core jsm::log | bit-exact (40000) | bit-exact (40000) | 933 / 40000 differ |

## Packing, per case

`equal` = every double bit-identical; `near` = same surviving cells, max |Δ| ≤ 0.000001 µm/rad; `DIVERGES` = different cells survive or |Δ| > tol.

| case | cand | removed (js) | native raw | native packed | wasm raw | wasm packed | native_stdmath raw | native_stdmath packed | ms js | ms native | ms wasm | ms native_stdmath |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c00 | 65 | 2 | equal | equal | equal | equal | near 7.1e-15 | near 1.4e-14 | 585.2 | 16.3 | 31.8 | 17.7 |
| c01 | 69 | 2 | equal | equal | equal | equal | near 7.1e-15 | DIVERGES (cell set) | 549.6 | 18.7 | 32.2 | 19.7 |
| c02 | 69 | 2 | equal | equal | equal | equal | near 7.1e-15 | near 2.8e-14 | 600.7 | 20.6 | 35.3 | 23.1 |
| c03 | 78 | 0 | equal | equal | equal | equal | near 7.1e-15 | near 5.7e-14 | 692.6 | 25.0 | 38.8 | 26.0 |
| c04 | 69 | 3 | equal | equal | equal | equal | near 3.6e-15 | near 2.8e-14 | 947.3 | 23.6 | 35.2 | 22.4 |
| c05 | 72 | 1 | equal | equal | equal | equal | near 3.6e-15 | DIVERGES (cell set) | 998.8 | 23.1 | 35.3 | 22.4 |
| c06 | 63 | 2 | equal | equal | equal | equal | near 2.2e-16 | near 3.2e-14 | 773.8 | 20.4 | 36.7 | 20.8 |
| c07 | 62 | 1 | equal | equal | equal | equal | near 7.1e-15 | DIVERGES (cell set) | 828.7 | 18.1 | 34.4 | 17.3 |
| c08 | 82 | 2 | equal | equal | equal | equal | near 3.6e-15 | DIVERGES (cell set) | 1600.2 | 34.6 | 55.7 | 31.7 |
| c09 | 71 | 1 | equal | equal | equal | equal | near 7.1e-15 | near 2.8e-14 | 1140.4 | 27.0 | 40.7 | 24.4 |
| c10 | 73 | 2 | equal | equal | equal | equal | near 3.6e-15 | equal | 1249.5 | 30.6 | 49.9 | 32.2 |
| c11 | 62 | 1 | equal | equal | equal | equal | near 3.6e-15 | equal | 669.6 | 19.0 | 32.2 | 19.0 |
| c12 | 62 | 1 | equal | equal | equal | equal | near 2.2e-16 | near 4.5e-13 | 901.0 | 19.3 | 29.4 | 19.6 |
| c13 | 70 | 2 | equal | equal | equal | equal | near 3.6e-15 | equal | 892.9 | 25.7 | 36.7 | 24.1 |
| c14 | 73 | 4 | equal | equal | equal | equal | near 7.1e-15 | equal | 931.4 | 29.5 | 45.1 | 28.5 |
| c15 | 67 | 0 | equal | equal | equal | equal | near 3.6e-15 | near 2.3e-13 | 814.5 | 21.8 | 33.4 | 20.4 |
| c16 | 64 | 2 | equal | equal | equal | equal | near 7.1e-15 | equal | 1414.5 | 25.0 | 34.8 | 22.6 |
| c17 | 64 | 2 | equal | equal | equal | equal | near 1.8e-15 | equal | 1342.5 | 23.6 | 28.5 | 19.2 |
| c18 | 55 | 0 | equal | equal | equal | equal | near 3.6e-15 | near 4.0e-15 | 810.6 | 13.9 | 18.9 | 12.8 |
| c19 | 76 | 7 | equal | equal | equal | equal | near 7.1e-15 | equal | 1999.3 | 35.5 | 51.7 | 32.8 |
| c20 | 67 | 2 | equal | equal | equal | equal | near 7.1e-15 | equal | 1663.7 | 30.6 | 42.9 | 27.5 |
| c21 | 71 | 5 | equal | equal | equal | equal | near 3.6e-15 | equal | 1507.6 | 28.5 | 36.8 | 26.1 |
| c22 | 78 | 4 | equal | equal | equal | equal | near 7.1e-15 | equal | 1654.0 | 36.0 | 43.9 | 28.5 |
| c23 | 75 | 9 | equal | equal | equal | equal | near 7.1e-15 | near 3.6e-12 | 1870.7 | 42.9 | 48.4 | 33.1 |
| c24 | 66 | 6 | equal | equal | equal | equal | near 7.1e-15 | equal | 1500.6 | 26.2 | 31.9 | 27.3 |
| c25 | 68 | 3 | equal | equal | equal | equal | near 7.1e-15 | equal | 1584.4 | 28.0 | 34.7 | 25.7 |
| c26 | 55 | 2 | equal | equal | equal | equal | near 3.6e-15 | equal | 1067.7 | 18.2 | 20.4 | 15.9 |
| c27 | 69 | 2 | equal | equal | equal | equal | near 7.1e-15 | equal | 1418.1 | 27.5 | 32.0 | 24.0 |
| c28 | 68 | 2 | equal | equal | equal | equal | near 3.6e-15 | equal | 1326.3 | 30.8 | 30.8 | 23.2 |
| c29 | 70 | 4 | equal | equal | equal | equal | near 7.1e-15 | equal | 1436.1 | 31.0 | 35.7 | 26.2 |
| c30 | 77 | 5 | equal | equal | equal | equal | near 1.4e-14 | equal | 2098.7 | 40.6 | 46.6 | 37.0 |
| c31 | 68 | 1 | equal | equal | equal | equal | near 3.6e-15 | equal | 1331.4 | 25.6 | 29.6 | 25.0 |
| c32 | 67 | 7 | equal | equal | equal | equal | near 7.1e-15 | equal | 1490.5 | 25.9 | 31.8 | 26.1 |
| c33 | 68 | 0 | equal | equal | equal | equal | near 7.1e-15 | equal | 1407.4 | 25.4 | 30.6 | 27.4 |
| c34 | 55 | 1 | equal | equal | equal | equal | near 3.6e-15 | equal | 934.2 | 17.3 | 19.3 | 17.9 |
| c35 | 130 | 57 | equal | equal | equal | equal | near 1.4e-14 | equal | 5358.9 | 117.9 | 152.0 | 120.7 |
| c36 | 124 | 44 | equal | equal | equal | equal | near 7.1e-15 | equal | 2000.0 | 105.0 | 132.1 | 103.3 |
| c37 | 113 | 21 | equal | equal | equal | equal | near 7.1e-15 | equal | 2712.8 | 85.0 | 99.7 | 78.9 |
| c38 | 122 | 23 | equal | equal | equal | equal | near 1.1e-14 | equal | 2893.9 | 97.3 | 116.4 | 85.8 |
| c39 | 115 | 19 | equal | equal | equal | equal | near 7.1e-15 | equal | 4264.0 | 89.0 | 105.8 | 78.1 |
| c40 | 62 | 27 | equal | equal | equal | equal | near 3.6e-15 | equal | 466.0 | 12.7 | 13.9 | 11.3 |
| c41 | 62 | 28 | equal | equal | equal | equal | near 7.1e-15 | equal | 481.5 | 13.3 | 14.4 | 10.6 |
| c42 | 70 | 26 | equal | equal | equal | equal | near 7.1e-15 | equal | 502.5 | 15.0 | 17.4 | 12.6 |
| c43 | 67 | 21 | equal | equal | equal | equal | near 3.6e-15 | equal | 362.6 | 12.1 | 13.6 | 10.4 |
| c44 | 54 | 14 | equal | equal | equal | equal | near 3.6e-15 | equal | 227.3 | 8.2 | 9.4 | 7.0 |
| c45 | 174 | 117 | equal | equal | equal | equal | near 7.1e-15 | equal | 11053.9 | 480.5 | 399.8 | 362.9 |
| c46 | 174 | 126 | equal | equal | equal | equal | near 7.1e-15 | equal | 9620.3 | 864.8 | 382.3 | 690.5 |
| c47 | 177 | 126 | equal | equal | equal | equal | near 7.1e-15 | equal | 8106.6 | 448.3 | 398.9 | 715.2 |
| c48 | 173 | 120 | equal | equal | equal | equal | near 7.1e-15 | equal | 7278.3 | 352.7 | 424.8 | 803.4 |
| c49 | 162 | 112 | equal | equal | equal | equal | near 7.1e-15 | equal | 7965.4 | 304.0 | 467.9 | 521.0 |

## Summary

- **native**: 50 equal, 0 near, 0 diverging (of 50)
- **wasm**: 50 equal, 0 near, 0 diverging (of 50)
- **native_stdmath**: 0 equal, 46 near, 4 diverging (of 50)

**FAIL**

## WASM core module size (C ABI only)

| | raw | gzip -9 | base64(raw) | base64(gzip) |
|---|---|---|---|---|
| insiliscope.wasm | 29.6 KB | 12.8 KB | 39.5 KB | 17.0 KB |
| Emscripten JS glue | 9.4 KB | 3.2 KB | | |
