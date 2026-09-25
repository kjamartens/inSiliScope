# Notes for webSMLM: porting Zernike-aberration PSFs

**Audience**: a future Claude session working in `C:\Users\koen-\Documents\GitHub\webSMLM`
(single-file client-side app, `webSMLM.html`). Written from *this* repo
(`demoCam_SMLM_MM`, a C++ Micro-Manager device adapter) right after
finishing its own Zernike-aberration PSF feature there, as a handoff of
what's directly reusable. Not a plan to execute here — re-scope against
whatever webSMLM actually needs at the time.

**Goal**: give webSMLM's synthetic-data simulator the ability to render
emitters with an arbitrary Zernike-aberrated PSF (astigmatism, coma,
trefoil, spherical, ...), the same OSA/ANSI-indexed coefficient model this
project just built — not a scalar-sigma shortcut, the actual pupil-phase
physics.

## 1. What demoCam_SMLM_MM built (the part that's portable)

Full details: [`../vectorial-psf-plan.md`](../vectorial-psf-plan.md)
("Step 5") and this repo's `CLAUDE.md` ("Vectorial PSF feature" section,
esp. the Step 5 Gotchas). The physics, stripped of this project's C++/Java
plumbing:

- **Coefficients**: 15 values, **OSA/ANSI single Zernike index 0–14**
  (covers every mode up to 4th radial order): 0 piston, 1 tip, 2 tilt,
  3 oblique astigmatism, 4 defocus, **5 vertical astigmatism**, 6 oblique
  trefoil, 7 vertical coma, 8 horizontal coma, 9 vertical trefoil,
  10 oblique quadrafoil, 11 oblique secondary astigmatism, 12 primary
  spherical, 13 vertical secondary astigmatism, 14 vertical quadrafoil.
  Each coefficient is in **waves** (unnormalized convention — see below).
  Full mapping + derivation:
  [`../../DeviceAdapter/SMLMDemoCam/Simulation/SMLMZernike.h`](../../DeviceAdapter/SMLMDemoCam/Simulation/SMLMZernike.h)'s
  doc comment.
- **Index → (n, m)**: standard OSA formula `j = n(n+1)/2 + l`, where `n` is
  the radial order and `l` (0..n) maps to azimuthal order `m = -n + 2*l`.
  Search upward over `n` for the `(n, l)` pair matching `j`. Ported here
  from EPFL's `psf_generator` (`utils/zernike.py`, `index_to_nl`) — see
  `indexToNM` in `GibsonLanniZernikePSF.java`.
- **Zernike value** `Z_j(rho, phi)` for `rho` in `[0,1]`: radial polynomial
  `R_n^m(rho) = sum_{k=0}^{(n-m)/2} (-1)^k * C(n-k, k) * C(n-2k, (n-m)/2-k) * rho^(n-2k)`
  (`m = |l|`), times `cos(m*phi)` if `l >= 0`, else `sin(m*phi)`. This is
  the **unnormalized** convention (no `sqrt((2n+2)/(1+delta_m0))`
  orthonormality factor) — a coefficient of 1.0 means "one full wave of
  `R_n^m(1)` peak amplitude," not "one wave RMS." See `zernikeRadial`/
  `zernikeValue` in `GibsonLanniZernikePSF.java` for the direct
  implementation (~25 lines, trivially portable to JS as-is).
- **Pupil phase**: `phase(rho, phi) = 2*pi * sum_j coeffs[j] * Z_j(rho, phi)`,
  plus (for through-focus/defocus behavior) a physical defocus/index-
  mismatch OPD term — in this project that's the Gibson-Lanni term
  (sample/immersion index mismatch + working-distance offset); for a
  webSMLM port the simplest physically standard choice is plain scalar
  defocus, `OPD(rho, z) = z * sqrt(1 - (NA*rho/n)^2)` (`n` = medium
  index), which is exactly what "index 4 (defocus)" already represents as
  a Zernike term too — you can fold z-dependence entirely into the
  Zernike-4 coefficient (`c4 * z`-scaled) rather than adding a second,
  separate OPD term, which is simpler and keeps one code path for "how z
  enters the phase."
- **PSF from pupil**: intensity at image-plane offset `(x, y)` is
  `|integral over the pupil disk of amplitude(rho,phi) * exp(i * (phase(rho,phi) + k*NA*rho*(x*cos(phi)+y*sin(phi)))) * rho dRho dPhi|^2`
  — a direct 2D numerical quadrature (this project uses a fixed 20×40
  `rho`/`phi` grid — tuned down from an initial 32×64 with no measurable
  accuracy loss against a zero-aberration regression check, see
  `GibsonLanniZernikePSF.java`'s class Javadoc). **No FFT** — brute-force
  double sum per output pixel. Computed once per parameter change (or, in
  a per-emitter-z simulator, once per distinct z-slice needed) and reused,
  never recomputed per emitter/frame.
- **Deliberately scalar** (no vectorial dyadic/polarization terms) so that
  all-zero coefficients reduce to a plain diffraction-limited PSF exactly
  — keep this property in any JS port too; it's what makes "did I break
  the unaberrated case" a cheap, meaningful check as you go.

**Not portable, don't bring any of this over**: the embedded-JVM/JNI
bridge, the GPL PSFGenerator jar, the MM device-property plumbing. All of
that exists solely because this project's C++ device adapter needed to
call into a pre-existing **Java** optics library. webSMLM is pure
client-side JavaScript with no native/JVM dependency at all — there is no
analogous "existing big library" to bridge to, so you'd just write the
(fairly small) math above directly in JS, no bridge needed.

## 2. Where this plugs into webSMLM

Read `webSMLM.html` directly before touching anything — line numbers below
are from this project's read of it, but the file changes.

- **Simulator**: `generateSynthetic()`, around line 3458. Right now every
  emitter renders as an **isotropic, fixed-`sigma=1.3px`** Gaussian (the
  hardcoded `sigma=1.3` at line ~3464; render loop at line ~3516-3519:
  `A*Math.exp(-((x-mx)**2+(y-my)**2)/(2*sigma*sigma))`). There is no
  per-emitter Z anywhere in the synthetic-data path today. Introducing
  Zernike PSFs here means:
  1. Give each emitter a Z (new per-event field, alongside the existing
     `x,y,tStart,tEnd`), from a new `PARAMS` entry (range/distribution),
     following the naming convention of existing `PARAMS.simulation_*`
     entries around line 2711-2715.
  2. Add `PARAMS` entries for the 15 Zernike coefficients (or a smaller
     curated subset + a preset dropdown — this project's own
     `PsfZernikePreset` in `SMLMDemoCamera.cpp`/`Simulation/SMLMZernike.cpp`
     is a reasonable model: `AstigmatismWeak/Moderate/Strong`,
     `ComaWeak/Strong`, `SphericalWeak/Strong`, etc., with magnitudes in
     the ~0.07-0.3 wave range — see that file for the values and their
     "not paper-sourced, order-of-magnitude estimates" caveat, worth
     repeating verbatim in any webSMLM UI).
  3. Replace the fixed-sigma Gaussian render with the pupil-quadrature PSF
     above, evaluated on an oversampled kernel and cached per distinct
     (coefficients, z) combination actually used in the frame set — not
     recomputed per emitter instance. NA, wavelength, and pixel size (nm)
     already exist as sim parameters to plug into `k = 2*pi/wavelength`
     and the pupil radius.
- **Existing elliptical-Gaussian analysis code this can be validated
  against**: `gaussianFitElliptical`/`gaussianFitEllipticalFixedXY`
  (~line 4955, 4996) fit independent `sigma_x`/`sigma_y` per spot;
  `calibrationCore` (~line 6936) fits **local quadratics**
  `sigma_x(z)`/`sigma_y(z)` from a real bead z-stack (`robustQuad`, not a
  fixed physical formula) plus a phasor-ratio z model. A Zernike-astigmatism
  synthetic stack (coefficient 5 nonzero, z-dependent as above) is a
  direct, physically-grounded way to generate ground truth for exercising
  that whole pipeline — the elliptical `sigma_x`/`sigma_y` shape the
  fitter expects falls out of the Zernike-5 pupil phase naturally near
  focus, it doesn't need to be special-cased.

## 3. Implementation gotchas from this project's Zernike work

- **OSA index numbering is easy to get off by one.** This project's own
  planning docs originally said "index 6 = vertical astigmatism"; the
  actual formula puts it at **index 5** (index 6 is oblique trefoil).
  Verify any index you hardcode (presets, defaults, docs, UI labels)
  against the `j = n(n+1)/2 + l` formula directly, not against prose or
  memory — that mistake propagated through two other planning docs here
  before being caught.
- **A "vectorial"/"full 2D" reference implementation's axisymmetric
  sibling can silently drop the exact feature you're porting.** EPFL's
  `psf_generator` (MIT, <https://github.com/Biomedical-Imaging-Group/psf_generator>,
  the algorithm reference used here) has both a `VectorialSphericalPropagator`
  and a `VectorialCartesianPropagator`. The spherical one looks like the
  obvious "vectorial" reference but its Zernike helper explicitly warns
  and **drops any non-axisymmetric term** (astigmatism, coma, trefoil —
  everything except piston/defocus/spherical) because its whole
  parameterization assumes an axisymmetric pupil. Only the *Cartesian*
  propagator (full 2D `(kx,ky)` pupil) supports general Zernike terms —
  that's the one to read for reference, and the one this project's
  `GibsonLanniZernikePSF.java` was actually ported from. Check this kind
  of scope-narrowing in a "general-looking" reference class before
  assuming its docstring's claimed generality holds all the way down.
- **If you parallelize this (Web Workers, since it's a real cost —
  `size^2 * n_rho * n_phi` per PSF instance), initialize worker state
  explicitly.** This project's Java port hit exactly this bypassing its
  underlying execution framework to parallelize manually: it skipped a
  `live=true` initialization step the framework normally did first,
  silently producing all-zero output for every single plane, with no
  exception at all. Caught only by explicitly checking output wasn't
  empty after the "optimization" — add that kind of check as you
  parallelize, don't trust "it ran without error."
- **Performance is dominated by kernel size × pupil-grid resolution, not
  by the Zernike math itself.** At default-ish settings this project's
  version originally computed kernels hundreds of pixels across × 70+
  Z-planes and took *minutes* even after parallelizing across CPU cores —
  see `CLAUDE.md`'s Step 5 Gotchas for the exact numbers and the two fixes
  that mattered (multithreading across independent PSF instances, and
  trimming the pupil quadrature grid from 32×64 to 20×40 with no visible
  accuracy loss). In a browser tab there's no free multithreading
  (Web Workers only if you explicitly add them), so keep the oversampled-
  kernel size and the number of distinct cached (coefficients, z)
  combinations modest by construction, especially before adding any
  worker-based parallelism.

## 4. Reference map

**In `demoCam_SMLM_MM`** (this repo):

- `docs/vectorial-psf-plan.md` — full step-by-step plan/history; see
  "Step 5" for the Zernike work specifically, including a worked
  explanation of why the Cartesian (not Spherical) `psf_generator`
  propagator was the right reference.
- `CLAUDE.md` — "Vectorial PSF feature" section: architecture summary,
  the `PsfZernikeCoefficients`/`PsfZernikePreset` properties, and two
  Gotchas subsections (step 1, step 5) with the concrete bugs hit.
- `DeviceAdapter/SMLMDemoCam/Simulation/SMLMZernike.h` — OSA index table
  doc comment (source of truth for index→mode mapping) and the preset
  coefficient values with their sourcing caveats.
- `DeviceAdapter/SMLMDemoCam/Simulation/psfbridge-java/psfbridge/GibsonLanniZernikePSF.java` —
  the actual portable math: `indexToNM`, `zernikeRadial`, `zernikeValue`,
  and the pupil-quadrature-to-pixel-intensity loop (`PlaneJob.process()`).

**In `webSMLM`**:

- `webSMLM.html` line ~3458 (`generateSynthetic`) — simulator to extend.
- `webSMLM.html` line ~3464/3516-3519 — current fixed-`sigma=1.3`
  isotropic Gaussian render, to be replaced.
- `webSMLM.html` line ~4955/4996 (`gaussianFitElliptical`/`...FixedXY`) —
  existing elliptical-Gaussian fitter to validate synthetic astigmatic
  Zernike data against.
- `webSMLM.html` line ~6936 (`calibrationCore`) — existing 3D calibration
  pipeline (local-quadratic `sigma_x(z)`/`sigma_y(z)` fit + phasor-ratio
  model) to exercise end-to-end with synthetic ground truth.
- `webSMLM.html` line ~2445-2446, ~2711-2715 — existing `PARAMS` entries,
  for naming/style convention when adding new coefficient/z parameters.

**External**: [EPFL `psf_generator`](https://github.com/Biomedical-Imaging-Group/psf_generator)
(MIT) — algorithm reference for the Zernike/pupil math.
