# Optics and PSF

## Conventions

- The `ZStage` position is the **focal plane height**; an emitter's defocus is \(z_{emitter} - Z\). Positive \(Z\) moves
  focus up through the sample, like a real focus drive. For `CellField`, \(Z = 0\) puts the coverslip in focus (the stage
  starts at 0.5 um).
- The camera pixel size (`General_PixelSizeNm`, default 100) and the PSF are independent; the PSF kernel is oversampled and
  placed at the emitter's true sub-pixel position.

## PSF models (adapter)

| `PSFParam_PsfModel` | What it is |
|---|---|
| `Gaussian` | \(\sigma = 0.21\lambda/\mathrm{NA}\); no defocus law yet |
| `RichardsWolf` | Vectorial focus field, in-focus, no index mismatch (PSFGenerator) |
| `GibsonLanni` | Adds the sample/immersion index mismatch and depth (PSFGenerator) |
| `GibsonLanniZernike` (default) | Scalar Gibson-Lanni plus a pupil wavefront from OSA/ANSI Zernike modes 0-27 (this project, C++, chirp-Z; the same model as webSMLM) |

`GibsonLanniZernike` is a **scalar** model: a unit-amplitude pupil (no apodization, no polarization) whose phase is the
Gibson-Lanni optical path difference (sample depth and immersion defocus), the Zernike sum and an optional mask. It is the
same model as webSMLM's, computed in C++ (`Simulation/ZernikePsf.cpp`), identical to webSMLM's JavaScript to the last
float32 bit on the reference cases (ctest `zernike_psf`) and to the original Java class (`GibsonLanniZernikePSF.java`, kept
as a reference) to ~1e-12 relative L2. It needs no Java; only `RichardsWolf` and `GibsonLanni` run in PSFGenerator's JVM.

The Zernike model evaluates the pupil-to-image integral on a Cartesian pupil grid with a separable 2D **chirp-Z (Bluestein)
transform**. Other pieces: the double-helix mask (`PsfMaskType = DoubleHelix`, Gauss-Laguerre modes), the Gibson-Lanni focal
shift for depth (the z stack is centred at \(t_{i0} - d\,n_i/n_s\)), and presets of aberrations (astigmatism, coma, spherical,
trefoil, saddle point, extended-range engineered PSFs) taken from webSMLM.

Placement: `Nearest` (box average), `Linear`, `Cubic` (Catmull-Rom, default) or `Fft` (Fourier shift, slow) sample the
oversampled kernel at the true emitter position. Kernel planes are normalised to sum 1 with precomputed block sums, so each
camera pixel is one interpolation.

A Cramer-Rao bound summary is logged to the Micro-Manager core log after each kernel computation.

!!! warning "Why the evaluator choice matters"
    A direct polar quadrature with 40 azimuthal samples aliased beyond ~1.6 um and made every emitter 17-20% too dim on
    the default kernel; it was removed. Chirp-Z is the only evaluator.

## PSF in the CLI and the viewer

The command line and the viewer use the same PSF as the adapter: `GibsonLanniZernike` by default (the same C++ code and
kernel cache), with options named after the `PSFParam_` properties (`psf-model`, `psf-zernike-preset`, `zern.<j>`,
`psf-mask`, `psf-oversampling`, `psf-kernel-half-width-nm`, `psf-z-range-um`, `psf-z-step-um`, `psf-sample-index`,
`psf-working-distance-um`, `psf-sample-depth-nm`, `psf-interp`; `insiliscope_cli --help` lists them). `psf-model=0`
selects the Gaussian; `RichardsWolf`/`GibsonLanni` need PSFGenerator's JVM and exist only in the adapter. The viewer uses
a 3 um kernel half width (instead of 7 um) to save browser memory.

## WideField imaging

Dyes are binned per population into world-anchored z planes (`General_WideFieldZPlaneNm`, default 25 nm) on a grid of
`General_WideFieldUpscaling` (1-4) cells per pixel. Each dye plane is convolved with the PSF plane at its defocus (dye planes
go to the two neighbouring PSF planes with linear weights, i.e. a linearly z-blended PSF), by FFT, then cropped and binned to
camera pixels:

\[ I(x,y) = \sum_{z} (n_z * \mathrm{PSF}_{z - Z})(x,y) \]

Implementation points that matter for accuracy and speed:

- the grid and dye tiles are anchored to the world, not the camera, so the same dyes bin the same way from every stage pose;
- the FFT size is the smallest \(2^a3^b5^c\) (multiple of 8) whose wrapped part misses the FOV;
- kernel and per-dye-plane spectra are cached, so a focus change is a re-pairing of cached spectra (about 10 ms at 256 px);
- a sub-cell stage move is a Fourier phase ramp, exact for band-limited images (pixel pitch \(\le \lambda/4\mathrm{NA}\));
- frames are weighted sums of precomputed images; bleaching uses a small basis of dose maps (or 12 Chebyshev maps) instead of
  per-frame spectra;
- coarser-grid "focus bands" exist but are gated at 0.1% rms error, which no sharp-pupil PSF currently meets.

A GPU path runs the same WGSL source on WebGPU in the viewer and on Direct3D 11 in the adapter (fp16 plane spectra, noise on the
GPU), self-checks against the CPU at startup and falls back to the CPU on any failure.
