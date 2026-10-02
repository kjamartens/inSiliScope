# BrightField (transmitted light) -- spec and status (2026-10-01, exploration branch)

The third imaging modality next to SuperRes and WideField: the cells of the CellField world seen in transmitted
light, from their refractive index. Code: `adapter/inSiliScope/Simulation/BrightfieldRender.{h,cpp}` (engine),
core ABI 6 `isc_optical_volume_in_window` (geometry), `ScopeMovie.cpp` (cli/viewer), the adapter's
`RenderBrightfieldStack` and live loop. Tests: ctest `brightfield`, `cli_tiff_bf`, `world_checks` (OpticalVolume),
`tests/web/viewer_bf_movie.mjs`, `tools/test_cellfield_stage.py` (BrightField + z sequence).

## Rule

**BrightField contrast comes only from structures the world simulates.** No invented texture, nucleoli, vesicles or
noise patterns stand in for missing biology. Today that is the cell body (cytoplasm height field), the nucleus
(ellipsoid) and the microtubules (12.5 nm tubes). Everything else is listed below and stays out until it is a real,
address-deterministic structure of the core (where it then also feeds SR/WF if it carries dyes).

## Survey of existing packages (why none is embedded)

| Package | What it does | Fit |
|---|---|---|
| [waveorder](https://github.com/mehta-lab/waveorder) (mehta-lab, BSD-3, Python/PyTorch) | partially coherent transfer functions (WOTF) for BF, phase, polarisation, fluorescence; reconstruction | the closest reference model; weak-object (first Born) only, so wrong for thick cells (~1-2 rad); a good independent check in the weak limit |
| [chromatix](https://github.com/chromatix-team/chromatix) (JAX) | differentiable wave optics: multislice / beam propagation, partially coherent sources, pupils | the model we follow (multislice + source sum); JAX, not embeddable in the adapter or WASM |
| TorchOptics (PyTorch) | differentiable Fourier optics, Gaussian-Schell partially coherent light | same physics family, Python |
| PyWolf (PyOpenCL) | partially coherent propagation (cross-spectral density) | no specimen model |
| holopy | Mie/DDA scattering, holography | validation of spheres, not cell fields |
| CytoPacq, SimuCell | synthetic cell image datasets | heuristic/fluorescence, no wave optics |
| BEM interference-microscopy toolbox (MATLAB) | coherent BF / iSCAT / dark field by boundary elements | far too heavy for a live camera |

All are Python/JAX/MATLAB; the project rule is one C++ implementation shared by the adapter (native) and the viewer
(WASM). So the standard model is ported, not linked.

## Model

**Specimen.** The core returns, per voxel of an `nx x ny x nz` grid, the volume fractions of cytoplasm `f_c` (cell
body minus nucleus), nucleus `f_n` and microtubule `f_m` (`isc_optical_volume_in_window`; z overlaps exact, `sub x sub`
column samples per voxel; pure geometry, no optical constants in the core). The renderer turns slice `k` (thickness
`dz`) into a phase screen and amplitude:

    phi_k = k0 dz [ f_c (n_c - n_m) + f_n (n_n - n_m) + f_mt (n_mt - n_c) ],   a_k = exp(-mu dz (f_c + f_n) / 2)

(a tube displaces cytoplasm, hence `n_mt - n_c`). Defaults (`SimType_CellField*`): medium 1.337, cytoplasm 1.345,
nucleus 1.345 (since 2026-10-01; were 1.360 / 1.355 -- cytoplasm and nucleus are both ~1.34-1.36 in measurements, e.g.
HeLa by Schuermann et al. 2016, and the lower values give realistic, weak BF contrast), microtubule 1.48
(protein-dense tube; an estimate), absorption 0 (unstained cells).

**Illumination.** Koehler, partially coherent: the condenser aperture (`General_BrightFieldCondenserNa`, default 0.55)
is sampled by `N` source points on an equal-area Fibonacci disk (Abbe's method), each a tilted plane wave snapped to the
FFT grid (so it is periodic). Monochromatic (`General_BrightFieldWavelengthNm`, 550 nm). Flat field.

**Propagation (multislice).** Light runs down (-z) through the cells to the coverslip. Per source: multiply by the top
screen, angular-spectrum step `exp(i kz dz)` in the medium (`kz = sqrt((n_m k0)^2 - k^2)`, evanescent waves dropped),
next screen, ... ; the exit spectrum just below the lowest screen is cached. One slice is the thin-object model; the
thin screen sits at the phase-weighted mean height (from 8 sub-slices), not at mid-height of the tallest cell.

**Imaging.** Detection pupil `|k| <= k0 NA` (`PSFParam_PsfNa`) with the PSF's Zernike aberrations
(`General_BrightFieldAberrations`, default on: same objective), refocused to the focal plane `Z` (the `ZStage`, + the
`SimType_CellFieldFocusHeightUm` offset) by `exp(i kz (z_obj - Z))`; intensity `|.|^2` per source, binned to camera
pixels, averaged over sources (empty field = 1). Photons = intensity x `General_BrightFieldPhotonsPerPxPerSec` x
exposure, then the usual camera chain (`ApplyNoiseChain`). Fluorescence background, haze and illumination profile
(`Background_*`, `FluoParam_Illum*`) do not apply.

**Grid.** `General_BrightFieldUpscaling` cells per pixel -- a minimum: `UpscaleFor` raises it until the pitch is
<= lambda / (4 n_medium) (0.103 um at 550 nm). That is enough: the propagating field is band-limited to |k| < k0 n, so
screen frequencies above 2 k0 n only scatter into evanescent waves, and a pitch of lambda / 4n samples the screen x field
product and the intensity without aliasing into the band (measured: a 2x or 3x grid changed the image by < 1% of the cell
contrast). Plus a margin (3-6 um by quality) whose outer half tapers the specimen phase to 0 (cosine), so the periodic
grid wraps without a phase jump. FFT sizes 2^a 3^b 5^c (`FftPlan1d`).

**Determinism.** Every source's image is a pure function of its index, written to its own slot and summed in source
order: bit-identical for any thread count (ctest `brightfield`). Frames differ only by camera noise.

**Cost.** Specimen propagation does not depend on focus: per new pose/setting `N x S x 2` FFTs; a focus change is one
inverse FFT per source (the exit spectra are cached up to 768 MB; above that they are recomputed per focus). Speed-ups
(2026-10-02, output unchanged): the slice transmittances, the slice propagator and the per-focus pupil x defocus are
computed once and shared by every source (they were per source: millions of sin/cos); the 2D FFTs work on 16 rows or
columns at a time in a cache-resident buffer (no full-array transposes), and only transform the columns of the
propagating band |kx| < k0 n where the rest is zero or discarded; the image's inverse only makes the FOV rows; the FFT
butterflies vectorize (WASM SIMD in the viewer). The cli/viewer keep one world and one scene across movies, so a repeat,
or a new frame count or noise setting, reuses the cells and the multislice.

## Quality: speed vs precision (`General_BrightFieldQuality`, cli/viewer `bf-quality`)

One number sets four knobs (each can be overridden: `bf-sources`, `bf-upscale`, `bf-sub`, `bf-slice-um`, and
`bf-margin-um`; MM `General_BrightFieldSources`/`Upscaling`/`GeometrySamples`/`SliceUm`, 0 / -1 = from the quality).

| Level | Sources | Geometry samples | Slice step | Margin | 256 px cli, 4 cores (cold) | 256 px viewer (1 core, cells built) | error / cell contrast |
|---|---|---|---|---|---|---|---|
| 1 fastest | 6 | 1 | thin (1 screen) | 3 um | 0.4 s | 0.1 s | 0.9-1.2 |
| 2 | 12 | 1 | 0.5 um | 3 um | 0.55 s | 0.5 s | 0.30-0.33 |
| 3 default | 24 | 2 | 0.5 um | 3 um | 0.7 s | 0.9 s | 0.25-0.28 |
| 4 | 48 | 2 | 0.25 um | 4 um | 1.9 s | 3.6-3.9 s | 0.13-0.15 |

All levels use the lambda / 4n grid (upscale 1 at 100 nm pixels). Error: rms difference of the noise-free image from a
reference (96 sources, 0.125 um slices, 4 samples, 6 um margin; level 5 in `BrightfieldQualityLevel`, for checks, not
exposed: cli/viewer/MM clamp the quality to 4) over the cell's own rms contrast, at `x = 30, y = 10` (a cell and its
nucleus), seed 42, focus 0.5 and 2 um. The slice step is the main lever (0.5 -> 0.125 um halves the error), then the
source count; the grid upscale and the geometry samples change nothing measurable. Before 2026-10-02 the levels were
12 sources / 1 um slices (2: error 0.45), 24 / 0.5 um on a 2x grid (3: 0.27, 3.0 s cli, ~12 s in the viewer) and
48 / 0.25 um on a 2x grid (4: 0.14, 10 s). The cli's cold time includes building the cells (~0.4 s; ~1.5 s in the
viewer, whose first level-3 movie at a new place takes 2.4 s); the viewer and MM keep them. Level 1 is a single thin screen: a flat lamella and a 6 um nucleus dome cannot both be at its one height, so
it misplaces focus by up to a few um; use >= 2 for z stacks.

## Camera: why the default per-pixel gain spread is 0.5%

BrightField puts thousands of photons in every pixel, so the camera's fixed pattern shows: the per-pixel gain spread
(`CamParam_GainStdPctPerPixel`, PRNU) multiplies the signal and is static. At its former default of 5% it was a
~345 ADU pattern at the default lamp (2000 photons/px/frame), above the ~160 ADU shot noise and far above the cells'
~85 ADU contrast, and it grew relative to shot noise with a brighter lamp. Real sCMOS PRNU is ~0.2-1%; the default is
0.5% since 2026-10-01 (all modalities). Also: at 0.25 photons/ADU and QE 0.85 the 16-bit range ends near 19k
photons/px/frame (`General_BrightFieldPhotonsPerPxPerSec` x exposure).

## Checks

- Weak phase grating, thin and 8-slice, coherent: image amplitude = first-order theory
  `-2 (eps/S) sum_k sin(dkz (z_k - Z))` to ~1e-5 (sign, size, focus), zero contrast in focus, mean 1.
- Empty field (no cells), partially coherent: exactly 1 at every pixel and focus.
- Real field: mean ~1 (energy), visible contrast, serial = parallel bit for bit, focus cache.
- Core: optical volume tiling (two halves = whole), 8 threads = 1, nz = 1 = mean over z, fractions in [0, 1], nucleus
  volume = sum of ellipsoid volumes (2%; measured 775.9 vs 776.0 um^3).
- Adapter (pymmcore-plus, Linux test build): lamp x2 = signal x2, defocus changes the image, live ~ precomputed,
  hardware z sequence (live and precomputed).
- Not done yet: a numeric comparison against waveorder's WOTF in the weak-phase limit (`pip install waveorder`; the
  grating test already checks the same transfer function analytically).

## Structures not simulated yet that will matter for BrightField

Excluded on purpose until they exist in the core (rule above). Roughly by expected BF impact:

1. **Nucleoli** -- the strongest intranuclear BF/phase contrast (n ~1.37-1.39, 1-3 um bodies).
2. **Lipid droplets** -- high index (~1.46-1.48), round, strongly scattering, very visible in BF.
3. **Vesicles, lysosomes, endosomes, granules** -- the main cytoplasmic BF "texture".
4. **Mitochondria** -- tubular network, visible in high-NA BF/DIC.
5. **Chromatin texture / nuclear envelope** -- the nucleus is a uniform ellipsoid today.
6. **Plasma membrane features** -- lamellipodia, ruffles, filopodia, blebs. The outline has a fractal wiggle (harmonics 6-64,
   D = 1.35, since 2026-10-01) and the height is a relaxed (crease-free) field, but there are no thin protrusions, ruffle
   folds or retraction fibres.
7. **Actin** -- cortex and stress fibres (DIC-visible bundles).
8. **ER, Golgi** -- diffuse; small contribution.
9. **Cell-cell junctions, other cell types, mitotic/rounded cells, cell debris** -- shape statistics of the field.
10. **NPCs, DNA** (planned core structures) -- sub-resolution; negligible in BF.
11. **Substrate and environment** -- coverslip defects, dust, ECM/coating texture, medium meniscus.
12. **Stains / absorbing dyes** -- the absorption term exists (`SimType_CellFieldAbsorptionPerUm`, all cell material);
    per-structure stains (e.g. H&E-like) need structures first.

Model limits (not structures): forward-only scalar multislice (no back-scatter, no polarisation, no vectorial pupil),
monochromatic, flat Koehler field (no `IlluminationPattern` for BF yet), no condenser aberrations or field diaphragm,
no index mismatch between medium and immersion for the refocus, no drift, CellField only; no phase-contrast or DIC
optics (both would be a pupil/source change on the same engine: annulus + phase ring, or two sheared sources).
