# Optics and PSF

## Conventions

- The `ZStage` position is the **focal plane height**; an emitter's defocus is \(z_{emitter} - Z\). Positive \(Z\) moves
  focus up through the sample, like a real focus drive. For `CellField`, \(Z = 0\) puts the coverslip in focus (the stage
  starts at 0.5 µm).
- The camera pixel size and the PSF are independent; the PSF kernel is oversampled and placed at the emitter's true
  sub-pixel position. In Micro-Manager the pixel size is the camera's sensor pixel / (objective magnification x
  `EmissionPath` magnification): 6.5 µm / (100 x 0.667) = 97.45 nm by default (Kinetix22, 100x objective; the 6.5 µm
  pixel from [[kinetix-datasheet](../references.md#kinetix-datasheet)], the 0.667 an *estimate*). The cli and viewer take
  it directly (`pixel-nm`, default 100).

## PSF models (adapter)

| `Renderer.PsfModel` | What it is |
|---|---|
| `Gaussian` | \(\sigma = 0.21\lambda/\mathrm{NA}\) [[zhang2007](../references.md#zhang2007)]; no defocus law yet |
| `RichardsWolf` | Vectorial focus field [[richards1959](../references.md#richards1959)], in-focus, no index mismatch (PSFGenerator [[kirshner2013](../references.md#kirshner2013)]) |
| `GibsonLanni` | Adds the sample/immersion index mismatch and depth [[gibson1992](../references.md#gibson1992)] (PSFGenerator [[kirshner2013](../references.md#kirshner2013)]) |
| `GibsonLanniZernike` (default) | Scalar Gibson-Lanni [[gibson1992](../references.md#gibson1992)] plus a pupil wavefront from OSA/ANSI Zernike modes 0-27 [[thibos2002](../references.md#thibos2002)] (this project, C++, chirp-Z; the same model as [webSMLM](https://github.com/kjamartens/webSMLM)) |

`GibsonLanniZernike` is a **scalar** model: a unit-amplitude pupil (no apodization, no polarization) whose phase is the
Gibson-Lanni optical path difference (sample depth and immersion defocus), the Zernike sum and an optional mask. It is the
same model as webSMLM's, computed in C++ (`Simulation/ZernikePsf.cpp`), identical to webSMLM's JavaScript to the last
float32 bit on the reference cases at webSMLM's 64-sample pupil (ctest `zernike_psf`; the default grid is finer, below) and to the original Java class (`GibsonLanniZernikePSF.java`, kept
as a reference) to ~1e-12 relative L2. It needs no Java; only `RichardsWolf` and `GibsonLanni` run in PSFGenerator's JVM.

The default PSF next to the unaberrated one, as the movie's kernel holds it:

<!-- fig:psf-sections -->

The Zernike model evaluates the pupil-to-image integral on a Cartesian pupil grid with a separable 2D **chirp-Z (Bluestein)
transform** [[rabiner1969](../references.md#rabiner1969), [bluestein1970](../references.md#bluestein1970)]. The grid
has as many samples as the kernel window needs (`Renderer.PsfPupilSamples`, cli/viewer `psf-pupil-samples`, 0 =
automatic): a sampled pupil makes the PSF repeat every \((M-4)\,\lambda/(2\,\mathrm{NA})\), so the
period must cover the window's diagonal, and the count is then raised to fill the transform's power-of-two length (free).
The default +/-7 µm window gets 184 x 184 samples, the viewer's +/-3 µm one 152.

!!! warning "Pupil sampling before 2026-10-07"
    The grid used to be 64 x 64 (webSMLM's `PSF_FFT_M`). Its period, ~14.6 µm in the red and ~10.9 µm in the green at
    NA 1.4, was shorter than the 14 µm wide kernel, so defocused light folded back into the window: WideField images
    showed stepped, square-ish bands of out-of-focus light (up to 3-4 sigma per frame), and near focus a camera pixel was
    off by up to 0.3 % of the emitter's photons (1.5 % of the brightest pixel). Against a 768-sample reference the
    automatic grid is within 1e-4 of an emitter's photons per camera pixel in the core and 1.3e-5 beyond 2 µm.
    `psf-pupil-samples=64` gives the old grid (and webSMLM's PSF, to the last bit).

The pupil grid measured against a finer one:

<!-- fig:psf-pupil -->

Other pieces: the Gibson-Lanni focal shift for depth (paraxial: the z stack is centred at \(t_{i0} - d\,n_i/n_s\)),
and presets of aberrations (astigmatism, coma, spherical, trefoil, saddle point, extended-range engineered PSFs) taken
from webSMLM (the single-mode amplitudes, 0.07-0.3 waves, and the default `MixedRealisticObjective` mix are
order-of-magnitude *estimates*, not values measured on an objective).

Every preset, its pupil wavefront and its PSF through focus:

<!-- fig:psf-presets -->

Placement: `Nearest` (box average), `Linear`, `Cubic` (Catmull-Rom [[catmull1974](../references.md#catmull1974)],
default) or `Fft` (Fourier shift, slow) sample the
oversampled kernel at the true emitter position. Kernel planes are normalised to sum 1 with precomputed block sums, so each
camera pixel is one interpolation.

The four placements against the exact Fourier shift:

<!-- fig:psf-interp -->

The kernel's oversampling (`Renderer.PsfOversampling`, cli `psf-oversampling`, default 6) sets how finely the kernel is sampled between camera pixel centres; its cost is the kernel's size and compute time:

<!-- fig:psf-oversampling -->

A Cramer-Rao bound summary [[ober2004](../references.md#ober2004)] is logged to the Micro-Manager core log after each
kernel computation.

!!! danger "Single-molecule PSFs are cut where their light is negligible (Renderer.Quality)"
    **A blinking emitter (dSTORM, PALM, DNA-PAINT) is not drawn with its whole +/-7 µm PSF.** Since 2026-10-07 each
    blink's splat leaves out every camera pixel that would get less than a fixed share of the emitter's photons: the
    **halo cut** (`Renderer.PsfHaloCut`, cli/viewer `psf-halo-cut`), set by `Renderer.Quality`:

    | Quality | Halo cut | What it means |
    |---|---|---|
    | Fast | 1e-5 | no left-out pixel would have had more than 1 photon in 100 000 of the emitter's |
    | **Realistic** (default) | **3e-6** | no left-out pixel would have had more than 3 photons in a million |
    | Exhaustive | 0 | the whole kernel, every pixel (the output before 2026-10-07) |

    The cut is per plane and per pixel row: it follows the PSF, a small disc in focus (~4.4 µm radius at 3e-6) and a
    wide one out of focus (~8.8 µm at 3.5 µm defocus; the square's corners at 3e-6 beyond ~3 µm). Pixels inside it get
    exactly the uncut values, **nothing is renormalized**: the left-out light (the far halo: ~0.5 % of a blink's
    photons at 3e-6, in focus and averaged over the planes; 1.1 % in focus and up to 1.9 % out of focus at 1e-5;
    measured 2026-10-08, the table below has the current values) is missing, not moved. On the default kernel the splat
    keeps ~60 % of the square's pixels at 3e-6 averaged over the planes (29 % in focus), 40 % at 1e-5 (13 % in
    focus). Measured live at 5 ms exposure (256 px, Iris Xe GPU, 2026-10-07): dSTORM ~64 -> ~91 fps
    (Realistic) -> ~104 fps (Fast), DNA-PAINT ~97 -> ~100 -> ~104 fps (its frame time is mostly elsewhere). The test is strict (the largest kernel block
    sum a pixel can read at any sub-pixel position, with a 1.5625x margin for cubic interpolation's overshoot; ctest
    `sr_render` checks every left-out pixel against it).

    What it does **not** touch: WideField labels, the mean-field and per-dye continuous populations (PALM pre states,
    the dSTORM initial ON phase), the free-imager background and BrightField keep the whole kernel. A dense
    continuous population sums the halos of very many dyes, and a cut there shows as edges of out-of-focus light.

    When it matters: summing very many frames of blinks to study the PSF's far wings (localization background models,
    out-of-focus haze), or comparing with webSMLM pixel for pixel. Use `Exhaustive` (or `PsfHaloCut` 0) for that.

The halo cut measured on one blink and on a movie:

<!-- fig:psf-halo -->

!!! warning "Why the evaluator choice matters"
    A direct polar quadrature with 40 azimuthal samples aliased beyond ~1.6 µm and made every emitter 17-20% too dim on
    the default kernel; it was removed. Chirp-Z is the only evaluator.

## PSF in the CLI and the viewer

The command line and the viewer use the same PSF as the adapter: `GibsonLanniZernike` by default (the same C++ code and
kernel cache), with options that mirror the adapter's PSF properties (`Objective.*`, `Renderer.Psf*`; `psf-model`, `psf-zernike-preset`, `zern.<j>`,
`psf-mask`, `psf-oversampling`, `psf-kernel-half-width-nm`, `psf-z-range-um`, `psf-z-step-um`, `psf-sample-index`,
`psf-working-distance-um`, `psf-sample-depth-nm`, `psf-pupil-samples`, `psf-halo-cut`, `psf-interp`; `insiliscope_cli --help` lists them). `psf-model=0`
selects the Gaussian; `RichardsWolf`/`GibsonLanni` need PSFGenerator's JVM and exist only in the adapter. The viewer uses
a 3 µm kernel half width (instead of 7 µm) to save browser memory; its blinks take the same halo cut (3e-6) within
that square.

## WideField imaging

Dyes are binned per population into world-anchored z planes (`Renderer.WideFieldZPlaneNm`, default 25 nm) on a grid of
`Renderer.WideFieldUpscaling` (1-4) cells per pixel. Each dye plane is convolved with the PSF plane at its defocus (dye planes
go to the two neighbouring PSF planes with linear weights, i.e. a linearly z-blended PSF), by FFT, then cropped and binned to
camera pixels:

\[ I(x,y) = \sum_{z} (n_z * \mathrm{PSF}_{z - Z})(x,y) \]

The steps on one field, slab by slab:

<!-- fig:wf-pipeline -->

Implementation points that matter for accuracy and speed:

- the grid and dye tiles are anchored to the world, not the camera, so the same dyes bin the same way from every stage pose;
- the FFT size is the smallest \(2^a3^b5^c\) (multiple of 8) whose wrapped part misses the FOV;
- kernel and per-dye-plane spectra are cached, so a focus change is a re-pairing of cached spectra (about 10 ms at 256 px);
- a sub-cell stage move is a Fourier phase ramp, exact for band-limited images (pixel pitch \(\le \lambda/4\mathrm{NA}\));
- frames are weighted sums of precomputed images; bleaching uses a small basis of dose maps (or 12 Chebyshev maps) instead of
  per-frame spectra;
- coarser-grid "focus bands" exist but are gated at 0.1% rms error, which no sharp-pupil PSF currently meets.

How fine the grid and the z planes need to be:

<!-- fig:wf-grid -->

WideField through focus:

<!-- fig:wf-defocus -->

A GPU path runs the same WGSL source on WebGPU in the viewer and on Direct3D 11 in the adapter (fp16 plane spectra, noise on the
GPU), self-checks against the CPU at startup and falls back to the CPU on any failure.

## BrightField imaging

Transmitted light through the cells (the `TransmittedLamp` shutter in Micro-Manager, `modality=1` in the cli and viewer; CellField only). Only simulated
structures make contrast: the core's optical volume gives, per voxel, the volume fractions of cytoplasm, nucleus and
microtubule, and the renderer turns them into refractive-index slices (`CellField.IndexMedium` 1.337,
`...Cytoplasm` 1.35, `...Nucleus` 1.35, `...Microtubule` 1.48, all *estimates*; optional absorption
`CellField.AbsorptionPerUm`). Measured at 540 nm, whole cells are 1.357-1.378 and isolated nuclei 1.343-1.353, the
nucleus below the cytoplasm, and PBS 1.335 [[steelman2017](../references.md#steelman2017)]: the defaults put both
compartments within the nuclei's range.
Structures that dominate real brightfield images (nucleoli, lipid droplets, vesicles, mitochondria...) are absent until
the world simulates them.

What the BrightField renderer sees of one field, and the image it makes:

<!-- fig:bf-scene -->

The model is scalar wave optics with partially coherent Koehler illumination (Abbe: the incoherent sum of the coherent
images of all source points [[zuo2017](../references.md#zuo2017)]): the condenser aperture
(`TransmittedLamp.CondenserNA`, default 0.4, an *estimate*) is sampled by equal-area source points; each tilted plane
wave is propagated down through the slices (multislice [[cowley1957](../references.md#cowley1957)] / beam propagation:
phase screen, then the angular-spectrum step \(e^{i k_z \Delta z}\) [[zuo2017](../references.md#zuo2017)]); the exit
field is refocused to the focal plane and filtered by the objective pupil (NA and the PSF's Zernike aberrations); the
intensities of all source points add:

\[ I(x,y;Z) = \frac{1}{N}\sum_{s=1}^{N} \left| \mathcal{F}^{-1}\!\left[ P(\mathbf k)\, e^{i k_z (z_{obj}-Z)}\, E_s(\mathbf k) \right] \right|^2 \]

Since the specimen propagation does not depend on focus, a focus change costs one inverse FFT per source point.
One number trades speed for precision (`Renderer.BrightFieldQuality`, cli/viewer `bf-quality`):

| Quality | Source points | Grid cells/pixel | Slice step | 256 px setup / per focus (12 threads) |
|---|---|---|---|---|
| 1 | 6 | 1 | one thin screen | 0.19 s / 6 ms |
| 2 | 12 | 1 | 0.5 µm | 0.3 s / 10 ms |
| 3 (default) | 24 | 2 | 0.5 µm | 0.4 s / 16 ms |
| 4 | 48 | 2 | 0.25 µm | 1.5 s / 33 ms |

The levels against a finer reference:

<!-- fig:bf-quality -->

Each knob can also be set alone (`bf-sources`, `bf-upscale`, `bf-sub`, `bf-slice-um`). A weak phase object in focus
shows almost no contrast; defocus brings it out with opposite signs above and below focus, as in a real microscope (the
phase transfer function is odd in defocus [[zuo2017](../references.md#zuo2017)]).
Details, the package survey and the list of missing structures: `spec/BRIGHTFIELD.md`.

BrightField through focus:

<!-- fig:bf-defocus -->

## Renderer.Quality: one knob for speed against precision

Micro-Manager's `Renderer.Quality` (`Fast`, `Realistic` (the default), `Exhaustive`) sets four of the knobs above together: the blink halo cut, the PSF oversampling, the WideField grid and the BrightField quality. The three levels side by side on the three kinds of image:

<!-- fig:quality-bundle -->
