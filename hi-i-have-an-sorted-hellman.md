# WideField imaging modality (cli/viewer + Micro-Manager)

## Context

Imaging is SR-only today: every frame renders the blinks the core's dye schedules switch ON
(`isc_events_in_window` -> `RenderPhotonImage` -> `ApplyNoiseChain`). You want a second
modality, **WideField**, where *all* labelled dyes emit at once. Drawing 2M+ dyes one by one is
infeasible, so the dyes are binned into z-planes and each plane is convolved with the PSF at its
defocus. Photophysics uses physical units. A general, world-anchored bleach state and a general
illumination pattern let you bleach, move away and come back, and swap in other excitation
patterns later. A new setting `ImagingModality` (`SuperRes` default, `WideField`) appears in the
viewer's movie panel, the cli, and MM.

Decisions made with you:

- **Units.** Excitation flux in photons/um^2/s (at pattern value 1), an extinction coefficient
  (-> absorption cross-section), a quantum yield (0.7), a photon budget (5000 emitted photons per
  dye). The bleach rate follows from these; there is no separate bleach time. The default flux
  gives t1/2 = 30 s.
- **Photon accounting.** The budget counts *emitted* photons. The camera receives emitted x NA
  collection efficiency, then QE in the noise chain.
- **The labelling split is respected.** Bleaching-labelled dyes decay; non-bleaching
  (DNA-PAINT-like) dyes stay at brightness 1. With the defaults (0% bleaching / 70% non-bleaching)
  nothing visibly bleaches until `LabelingPctBleaching` is raised.
- **Illumination is a general, modality-neutral pattern** (boilerplate), anchored to the
  objective. For now it is square unity over the FOV. WideField already drives excitation and
  bleaching from it, so a new pattern needs no renderer change. No beam properties now.
- **MM supports both live and precomputed stack**, for the CellField pattern only. Other patterns
  log once and render as SR.
- **CPU now, GPU next.** The FFT convolution is multi-threaded on the CPU. The existing D3D11
  shader is a per-emitter splat and can't be reused; a D3D11 FFT-convolution path is the
  follow-up step.
- **Stacks start from a fresh sample.** They are reproducible and never touch the live bleach
  map. Live mode keeps its own persistent, world-anchored map.

## Model

Per dye at sample position r, with pattern value I(r) (peak 1) and peak flux Phi:

```text
sigma   = ln(10) * 1000 * eps / N_A          [cm^2]  = 3.8235e-13 * eps  [um^2]
k_em(r) = QY * sigma * Phi * I(r)            emitted photons / s  (unbleached dye)
eta     = 0.5 * (1 - sqrt(1 - (NA/n)^2))     collection efficiency (0.307 at NA 1.4, n 1.518)
dose D  = integral of k_em dt                emitted photons so far (BleachField stores this)
S       = exp(-D / B)                        surviving fraction, B = photon budget
```

Photons reaching the camera in one frame from a voxel with `nb` bleaching and `np` persistent dyes.
D0 is the dose at frame start and dD = k_em * t_exp:

```text
bleaching:  nb * eta * B * exp(-D0/B) * (1 - exp(-dD/B))   (exact frame integral, not a start sample)
persistent: np * eta * dD
```

- Defaults are eps = 270,000 M^-1 cm^-1 (sigma = 1.03e-7 um^2), QY = 0.7, B = 5000.
  - The default Phi is 1.6e9 photons/um^2/s (about 0.05 W/cm^2 at 640 nm), which gives
    t1/2 = B ln2 / k_em = 30.0 s.
  - That is about 1.8 photons/dye reaching the camera per 50 ms frame.
- Summing over voxels gives the per-plane source maps A_k (upscaled grid, pitch = pixel / upscale):

  ```text
  img = sum_k  A_k (*) PSF(z_k - focus)  -> crop FOV -> sum upscale x upscale blocks
  ```

  The existing background (map x `FluoParam_IllumProfile` field x fade) is added on top.
- The **Poisson draw happens once per camera pixel on the summed expectation**, in the existing
  `ApplyNoiseChain` (QE, dark current, Poisson, read noise, gain, offset, same counter-based
  seeds). A sum of independent Poissons is Poisson of the sum, so this has the same distribution
  as sampling each plane (or sub-pixel) and adding the results.
- Blink kinetics are unused in WideField. Drift is not applied in v1 (logged once if non-zero).

## 1. Core: ABI 5, a z-resolved, population-selectable density query

`isc_density3d_in_window(w, x0,y0,x1,y1, zMin,zMax, nx,ny,nz, populations, float* out)`:
labelled-dye counts on an nx x ny x nz grid, `out[(k*ny+iy)*nx+ix]`, with plane k spanning
[zMin + k(zMax-zMin)/nz, ...). `populations` is a bitmask:
`ISC_POP_BLEACHING 1 | ISC_POP_PERSISTENT 2`. It returns the total count, or -1. It generalises
`isc_density_in_window` and answers the "fast z-lookup" question: the core bins millions of cached
dyes straight into voxels, and nothing crosses the ABI per dye.

- `core/src/world.{h,cpp}`: `World::Density3dInWindow` iterates `ForEachDyeBlock` directly (no copy
  through `SitesInWindow`), using the existing `WorldDye::persistent` flag.
- `core/src/capi.cpp`, `core/include/insiliscope/insiliscope.h`: the export and constants;
  `ISC_ABI_VERSION 5`. `core/CMakeLists.txt`: add it to `ISC_EXPORTS`.
- Bump the pinned ABI in `web/index.html:693` and `tests/parity/wasm_abi_smoke.mjs:86` (plus one
  call of the new export there). The block and embed tools read the header.
- `tests/parity/world_tests.cpp`, new `Density3d()` in `world_checks`:
  - summed over z it equals `isc_density_in_window`;
  - bleaching + persistent = all, voxel by voxel;
  - it matches hand-binning `SitesInWindow` by the persistent flag;
  - nz = 1 over a slab equals the 2D query with the same z limits.
- `spec/PORT.md`: document the query and the WideField use.

## 2. Engine (`adapter/inSiliScope/Simulation/`, no MM dependency)

**`Illumination.{h,cpp}` (new, modality-neutral boilerplate).**

```cpp
// Excitation pattern in the sample plane, anchored to the objective (stage moves slide the
// sample under it). Coordinates: um relative to the optical axis (= FOV centre).
class IlluminationPattern {
public:
   virtual ~IlluminationPattern() = default;
   virtual double At(double dxUm, double dyUm) const = 0;   // relative excitation, peak 1
   virtual void Support(double& x0, double& y0, double& x1, double& y1) const = 0; // 0 outside
   // Cell-centre samples on a grid (override for exact area averages).
   virtual void Sample(double x0Um, double y0Um, double pitchUm, unsigned nx, unsigned ny,
                       float* out) const;
};
class SquareIllumination : public IlluminationPattern { /* 1 inside a w x h rect, 0 outside */ };
```

- Built once per config by the camera and ScopeMovie (currently the FOV square).
- WideField reads k_em from it, and deposits dose over its whole support, so a wider pattern
  bleaches beyond the FOV with no renderer change.
- SR keeps its `FluoParam_IllumProfile` field for now (byte-identical); moving SR and the
  background onto the pattern is a later step.

**`Fft2d.{h,cpp}` (new).** In-place N x N complex<float> FFT, N = 2^k: iterative radix-2 with
precomputed twiddles, rows then columns via a cache-friendly transpose. No third-party dependency,
it stays small in the viewer's WASM, and its operation order is identical native vs WASM.
- `RealPairForward(a, b) -> A, B` packs a + i*b and splits the result by Hermitian symmetry.
- Inverse is normalised.

**`WidefieldRender.{h,cpp}` (new).**

- `WidefieldPhotophysics { excitationPhotonsPerUm2PerSec=1.6e9, quantumYield=0.7,
  photonBudget=5000, extinctionCoeff=270000 }`, with helpers `CrossSectionUm2()`,
  `EmissionRatePerSec(I)`, `HalfTimeSec(I)`, and `CollectionEfficiency(na, n)`.
- `WidefieldGridSettings { upscale=1, zPlaneNm=25 }`.
- `WidefieldDyeGrid` + `BuildWidefieldDyeGrid(CellFieldSource&, window, z slab, settings)`:
  - window = FOV grown by the SR margin (2 um, `kCellFieldMarginUm`), intersected with the
    pattern's support. For the square that is the FOV exactly: dyes outside are dark;
  - planes are world-anchored at multiples of zPlaneNm, so a focus move changes only PSFs;
  - pass 1 is a z histogram (nx = ny = 1) to find the occupied planes; pass 2 builds the 3D grid
    over just that range, per population, keeping memory at occupied planes only;
  - the z slab reuses `SimType_CellFieldZRangeUm` (the cli's `z-range-um`); 0 means "all" and is
    histogrammed over [-5, 50] um.
- PSF providers, kernel on the upscaled grid at a given defocus, normalised to sum 1, radius R:
  - **Gaussian** (cli, viewer, and the adapter's `Gaussian` model): sigma(defocus) from
    `WidefieldGaussianSigmaUm(defocusUm, lambdaNm, na, n)`. **Learn-by-doing TODO(human)**; see
    below.
  - **Vectorial** (`PsfKernelCache`): the kernel planes are resampled to the upscaled pitch.
    With oversampling divisible by upscale (default 6 works for 1, 2, 3), each upscaled cell is
    the sum of its (os/u)^2 oversampled cells, placed exactly as `SplatPsfKernel` centres them;
    otherwise the camera logs a warning and uses the largest valid upscale. The 25 nm dye planes
    are deposited onto the two neighbouring 100 nm kernel planes with linear weights. This is
    identical to convolving with a linearly z-blended PSF, at ~4x fewer FFTs.
- Convolution:
  - N = pow2 >= max(nx + m + R, 2R + 1) (m = grid margin beyond the FOV, in upscaled px);
  - per PSF plane j: one complex FFT of (A_j + i*P_j), split, then S += F(A_j) * F(P_j);
  - one inverse FFT per frame; clamp >= 0; crop; bin.
  - R: 3 sigma(max defocus) for the Gaussian, the kernel half-width for vectorial, capped at
    `PSFParam_PsfKernelHalfWidthNm` (cli option `wf-kernel-um`, default 7).
  - Native builds split the planes over threads (each thread keeps a partial S); WASM runs one
    thread.
- **Fast path:** the persistent population's spectrum S_nb is cached per pose/focus. It is static,
  so with the default labelling (0% bleaching) every frame is one scalar-free reuse plus an
  inverse FFT. The bleaching spectrum S_bl is cached with its per-column weight map. If the new
  weights are a scalar multiple of the cached ones (uniform pattern, sample not moved), the frame
  is `c * S_bl + S_nb`.
- `BleachField`: world-anchored map of emitted-photon dose (photons per dye) in sparse 256 x 256
  tiles, pitch = the WF pitch, cell i = floor(x / pitch). With equal pitch, grid columns map
  one-to-one to cells whatever the stage offset, so reads and deposits are consistent. It offers
  `DoseOver(window)`, `Deposit(pattern, axis position, dD per unit I)` over the pattern's support,
  and `Reset()`. A pitch change resets it (logged).

**`CellFieldSource.{h,cpp}`:** `Density3d(...)` wrapping the new ABI call.

## 3. cli + viewer

- `ScopeMovie.{h,cpp}`:
  - new options:
    - `modality`: 0 = SuperRes, 1 = WideField; the names are accepted via a small name-to-value
      hook used by `ParseScopeSpec` and the cli's `atof` path;
    - `wf-upscale` 1, `wf-plane-nm` 25, `wf-kernel-um` 7;
    - `wf-excitation-photons-per-um2-per-sec` 1.6e9, `wf-quantum-yield` 0.7,
      `wf-photon-budget` 5000, `wf-extinction-coeff` 270000;
    - `immersion-index` 1.518 (collection efficiency and the Gaussian defocus model);
  - the WideField branch of `RenderScopeMovie`: `SquareIllumination` over the FOV, grid, PSF at
    focus `z`, fresh sample with D0 = f * dD, the existing noise maps and seeds;
  - `ScopeMovieInfo` gains `dyes`; the description line records the modality and the derived
    t1/2 and photons/dye/frame.
- `cli/CMakeLists.txt`: add the new sources to `ISC_SIM_SRC`; ctest `cli_tiff_wf`
  (`--modality 1`, 64 px, 10 frames).
- `web/index.html` movie panel:
  - a `<select id="mv_modality">` (SuperRes / WideField) and WideField rows (upscale, plane nm,
    excitation flux, QY, budget, extinction coefficient) shown only for WideField;
  - `MOVIE_KEYS` extended; the info line shows the dye count and t1/2;
  - regenerate `web/insiliscope_module.js` (`node tools/embed_web_module.mjs`).

## 4. Micro-Manager adapter

New properties, following the CLAUDE.md naming convention:

| Property | Values / default |
| --- | --- |
| `General_ImagingModality` | `SuperRes` \| `WideField`, default `SuperRes` |
| `General_WideFieldUpscaling` | int 1-4, default 1 |
| `General_WideFieldZPlaneNm` | 5-500, default 25 |
| `FluoParam_WideFieldExcitationPhotonsPerUm2PerSec` | 0-1e13, default 1.6e9 |
| `FluoParam_WideFieldQuantumYield` | 0-1, default 0.7 |
| `FluoParam_WideFieldPhotonBudget` | 0-1e9, default 5000 (0 = never bleaches) |
| `FluoParam_WideFieldExtinctionCoeff` | 1e3-1e6 M^-1 cm^-1, default 270000 |

The numbers share one indexed handler (like `OnCellFieldNumber`/`cellField_[]`); each calls
`InvalidateStack()`. The collection efficiency comes from `PSFParam_PsfNa` and
`PSFParam_PsfImmersionIndex`. On every config the corelog records sigma, k_em at peak, t1/2, eta
and photons/dye/frame. Files: `InSiliScopeCamera.{h,cpp}`, `SMLMImageGeneration.cpp`,
`inSiliScope.vcxproj` + `.filters` (new `.cpp`s; the Linux script globs `Simulation/*.cpp`).

- **Stack** (`StackGenerationWorker`), WideField + CellField:
  - grid at the snapshot pose;
  - fresh sample: D0_f = f * dD(r), closed form, so frames stay independent and the existing
    thread-per-frame pool still applies, with scratch buffers per thread;
  - Z is read per frame as today, and the PSF is re-derived when it changes;
  - same noise seeds.
- **Live** (`LiveProducerLoop`):
  - persistent `BleachField` across frames and config changes; it resets on a world or seed
    change, or a pitch change;
  - per frame: D0 from the field, render, then deposit dD over the pattern's support;
  - the grid is rebuilt when the pose, slab or grid settings change (dyes are already cached and
    prefetched for the z column);
  - the fast path covers a stationary sample.
- `General_GpuStatus` reads "CPU (WideField: FFT convolution on the CPU; GPU path planned)".

**Follow-up (not in this step): the D3D11 WideField path.** A compute-shader FFT (radix-2 Stockham
in groupshared memory, row and column passes) plus a per-plane multiply-accumulate and the existing
HLSL noise chain, keeping the CPU path as the fallback and the parity reference. Built once the
timings from Verification step 6 show where the time goes.

## 5. Docs and plan

- `CLAUDE.md`: the new properties in the naming-convention lists, and a short "WideField modality"
  section covering:
  - the model and its units, the dose law and the split;
  - the illumination pattern (square unity for now), the fast path;
  - the limits: CellField only, no drift, CPU.
- `spec/PORT.md`: the density3d query plus the WideField pipeline.
- `PLAN.md`: your uncommitted edit replaced it with an ideas list, and I won't restore the old
  checklist. I'll only annotate the line "addition of regular fluorescence" with this step's
  status.
- Commits, one per step, none pushed: (1) core ABI 5 + tests, (2) engine + cli + viewer,
  (3) adapter, (4) docs.

## Learn-by-doing (TODO(human), as agreed)

`WidefieldGaussianSigmaUm(defocusUm, lambdaNm, na, n)` gives the width of the **emission PSF**
(the image of one dye) at a given defocus. It is not the excitation beam; that is the
IlluminationPattern. Today's Gaussian ignores z, so without this widefield would have no
out-of-focus haze in the cli/viewer. I'll leave a working placeholder (the in-focus sigma, z
ignored), marked `TODO(human)`, for you to replace, e.g. with:
- a Gaussian-beam-style law, sigma0 * sqrt(1 + (dz/zR)^2);
- a geometric defocus-cone term, sqrt(sigma0^2 + (dz*NA / (2 sqrt(n^2 - NA^2)))^2);
- or a blend.

## Verification

1. Core, native + WASM:
   - `cmake --build --preset msvc && ctest --test-dir build/msvc -C Release` (`world_checks` incl.
     `Density3d`, `golden_vectors`, `cli_tiff`, `cli_tiff_wf`, `emitter_density`, new
     `widefield`);
   - the same with `--preset wasm` under emsdk 6.0.10;
   - `node tests/parity/run.mjs`; `node tools/embed_web_module.mjs --check`;
   - `node tools/make_cellfield_block.mjs && node tests/block/check_cellfield_block.mjs`.
2. New ctest `widefield` (`cli/widefield_check.cpp`):
   - FFT vs a naive DFT (rel. err < 1e-5);
   - FFT convolution vs direct convolution;
   - photon conservation (an in-focus uniform grid far from the edges sums to
     dyes x eta x dD);
   - units: the default parameters give t1/2 = 30.0 s and 1.8 photons/dye/frame;
   - the bleaching frame integral vs numerical integration;
   - split respected: persistent-only frames are constant, bleaching-only frames decay with
     t1/2;
   - `SquareIllumination`: support, sampling, deposit outside the FOV = 0;
   - fast path == full path to 1e-5;
   - determinism.
3. **SR unchanged:**
   - `python tools/adapter_pixel_hash.py <dll dir>` identical before and after;
   - an SR `insiliscope_cli` TIFF identical before and after;
   - viewer SR movie == cli.
4. WideField cli/viewer parity: a viewer WideField movie compared with the cli for the same spec.
   Byte-identical is expected but may be "near" (libm cos/exp last-ulp); report which.
5. Adapter (Linux test build + `tools/test_cellfield_stage.py`, extended):
   - WideField properties present;
   - with 20% bleaching labelling, the mean signal decays with the predicted t1/2;
   - move the stage 30 um away and back: the region is still dim, and a fresh region is bright;
   - persistent-only stays constant.
6. Timings reported for the default 256 px FOV at upscale 1 and 2 (stack and live, full vs fast
   path). Then MSBuild on Windows, `test_insiliscope.py`, and a look in Micro-Manager by you.
