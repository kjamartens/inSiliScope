# Vectorial PSF simulation for demoCam_SMLM_MM (via PSFGenerator)

## Context

The plugin currently renders every emitter with an analytic, additive 2D
Gaussian ([`RenderGaussianPSF`](DeviceAdapter/SMLMDemoCam/Simulation/SMLMSimulation.cpp:13)),
with `sigmaPx` derived from wavelength/NA via a diffraction-limit
approximation ([`ComputePsfSigmaPx`](DeviceAdapter/SMLMDemoCam/SMLMImageGeneration.cpp:56)).
This is a scalar approximation with no diffraction-ring structure, no
polarization/vectorial effects, and no defocus/Z dependence.

Rather than re-implementing Richards & Wolf / Gibson & Lanni physics in C++,
this plan delegates PSF *computation* to EPFL's Java
[PSFGenerator](https://bigwww.epfl.ch/algorithms/psfgenerator/)
(GPL-3.0, [source](https://github.com/Biomedical-Imaging-Group/PSFGenerator)),
run as a **separately-downloaded, externally-invoked tool**. Per your
feedback, there is no image-file round trip anywhere: a small companion
Java "bridge" class (built by us, kept separate from PSFGenerator's own
source tree) calls PSFGenerator's existing in-memory compute API
(`PSF.getPlane(z)` → `double[]`, no `ImagePlus`/TIFF involved) and writes
the resulting float planes as a **raw binary stream to its stdout**, which
the C++ side reads directly off the child process's stdout pipe. No TIFF,
no temp files, no image-format parsing on either side — just a length-known
buffer of floats (nx, ny, nz are already known to the C++ side from the
request it sent, so no header/framing beyond that is needed). This keeps a
clean process boundary between this project's BSD-licensed C++ and
PSFGenerator's GPL-3.0 Java (invoked as an external tool, not linked into
this binary), which was the deciding factor against embedding a JVM via
JNI. The C++ device adapter's job stays what it already does well: caching
one oversampled PSF result and cheaply resampling/splatting it at every
emitter position, per frame, in Live mode.

**Key finding that shapes the Zernike step**: PSFGenerator's
`RichardsWolfPSF.java` computes a *radially-symmetric* PSF only — a 1D
radial lookup table (`KirchhoffDiffractionSimpson`) interpolated onto the
2D grid by radius, with "aberration" limited to the Gibson & Lanni
refractive-index-mismatch term. There is no azimuthal (phi) dependence
anywhere in that code path, so it structurally cannot represent most
Zernike modes (astigmatism, coma, trefoil, ...), which are not rotationally
symmetric. EPFL's newer, actively maintained Python package
[`psf_generator`](https://github.com/Biomedical-Imaging-Group/psf_generator)
(MIT license, PyTorch-based, successor project) *does* implement a full 2D
pupil-plane Richards-Wolf integral (`VectorialSphericalPropagator`) with a
`zernike_coefficients` parameter — but has no Gibson-Lanni model.
Per your decision: stay all-Java. Steps 1-3 use the stock PSFGenerator.jar
(Richards-Wolf and Gibson-Lanni, no aberration). The Zernike step requires
a **locally patched/forked build of PSFGenerator.jar** with a new
Zernike-capable PSF model class, whose math is ported from the Python
package's `VectorialSphericalPropagator` (referencing its published
algorithm — fresh Java code, not copied Python text). This is a real
engineering step (a new full 2D pupil integral), not a config toggle.

Confirmed design decisions from earlier discussion:
- Kernel recompute (subprocess call + raw-buffer read) happens **inline**
  on whichever thread already rebuilds cached live-mode state (same
  pattern as the existing `offsetMap` regen in `LiveProducerLoop`) — a
  brief stutter when changing a PSF parameter is acceptable.
- No fixed-dipole-orientation modeling — PSFGenerator's vectorial models
  are used as-is (unpolarized/isotropic detection, their standard
  behavior).
- The Z-stage step's Z control is a separate `MM::Stage` device
  (`SMLMDemoZStage`), linked to the camera via a small shared-state object.
- All external PSF computation stays **Java-only** (no Python dependency
  introduced), invoked as a subprocess (no embedded JVM/JNI), even for the
  Zernike step.

## Shared architecture (introduced in step 1, extended later)

New files:

- **`Simulation/PsfGeneratorBridge.h/.cpp`** (no MMDevice dependency, same
  style as `SMLMSimulation.h/.cpp`):
  - `PsfGeneratorRequest`: wavelength, NA, immersion index, pixel size,
    oversampling factor, kernel half-width (→ nx/ny), model
    (`RichardsWolf`/`GibsonLanni`), and (from step 2) z-range/z-step (→ nz,
    resAxial, both user-facing properties — see step 2), and (from the
    Zernike step) Zernike coefficients.
  - `BuildConfigFile(request, path)`: writes a small PSFGenerator
    parameters text file (just numeric config — not an image file). Exact
    config key names (e.g. the `psf-RW-NI` style seen in
    `RichardsWolfPSF.java`'s `settings.record(...)` calls) will be
    confirmed once, in step 1, by running the PSFGenerator GUI, setting the
    desired model/params, and inspecting/saving its persisted settings file
    as the concrete key template — do not guess these blind.
  - **`Simulation/psfbridge-java/`** (new small Java source tree, built
    separately from PSFGenerator itself): a `PsfBridgeMain.java` whose
    `main(String[] args)` takes the config file path, constructs the
    requested `PSF` subclass directly (e.g. `new RichardsWolfPSF()`, the
    same classes PSFGenerator's own `CollectionPSF` uses), calls
    `.process()`/`.getPlane(z)` for each z, and writes every plane's
    `double[]` (cast to `float`) as raw little-endian bytes to `System.out`
    — bypassing `ImagePlus`/`Converter`/TIFF entirely. Depends on
    PSFGenerator.jar's classes on its classpath (built against it, not
    copying its code) — build with a matching Ant target or plain `javac
    -cp PSFGenerator.jar`.
  - `RunPsfGeneratorBridge(javaPath, bridgeJarPath, psfGeneratorJarPath, configPath)`:
    spawns `java -cp <bridgeJar>;<PSFGenerator.jar> PsfBridgeMain <config>`
    as a synchronous child process (Windows `CreateProcess` with a pipe for
    stdout, matching this project's Windows/MSVC toolchain), reads
    `nx*ny*nz` floats off the pipe as they arrive, and waits for clean
    process exit.
  - `struct PsfKernelCache`: the oversampled plane(s) (now filled directly
    from the raw stdout buffer, no parsing step) + metadata, feeding the
    existing-style `SplatPsfKernel(img, w, h, cache, zIndex, xPx, yPx, totalPhotons)`
    that shifts by the emitter's fractional-pixel offset and box-bins the
    oversampled plane down to camera-pixel resolution at splat time — the
    "downsample to place everywhere" step. Each downsampled camera-pixel
    kernel is normalized to sum to 1 before scaling by `totalPhotons`, so
    photon-count conservation matches the current Gaussian renderer.
- `CSMLMDemoCamera` gains a `sim::PsfKernelCache psfCache_`, rebuilt inline
  whenever `liveConfigVersion_` changes and a PSF-relevant property was
  touched — reusing the *existing* `InvalidateStack()` mechanism; new PSF
  properties just call it like every other property already does. On
  subprocess failure (java/jars missing, bad config, nonzero exit, short
  read), log and fall back to the existing Gaussian renderer rather than
  breaking image acquisition.
- New properties: `PsfModel` (enum `Gaussian`/`RichardsWolf`/`GibsonLanni`,
  default `Gaussian` until step 1 is validated, then switchable for A/B
  testing), `PsfGeneratorJavaPath`, `PsfGeneratorJarPath`,
  `PsfBridgeJarPath`, `PsfImmersionIndex` — declared/registered following
  the exact pattern of `OnPsfWavelengthNm`/`OnPsfNa` in
  `SMLMDemoCamera.h`/`.cpp` and `SMLMImageGeneration.cpp`.

## Step 1 — In-focus 2D vectorial PSF via PSFGenerator + oversample/downsample plumbing

- Run PSFGenerator's GUI once to determine exact config keys for: model
  selection, NA, lambda, resLateral, nx/ny, nz=1 (or small, since
  `RichardsWolfPSF.checkSize` requires `nz >= 3` — check whether a thin
  stack of 3 with only the center plane used is required, or whether
  Gibson-Lanni has a looser constraint), and immersion index.
- Write `PsfBridgeMain.java` and build it against the stock
  `PSFGenerator.jar` (documented download/build steps for you).
- Implement `PsfGeneratorBridge` end-to-end for a single (in-focus) plane:
  build config → spawn bridge subprocess → read raw float planes off
  stdout → populate `PsfKernelCache`.
- Wire `SplatPsfKernel` into `RenderPhotonImage`'s per-`BlinkEvent` loop
  when `PsfModel != Gaussian`; keep `RenderGaussianPSF` for `Gaussian`.
- **Test**: switch `PsfModel` to `RichardsWolf` in Live mode at a high NA
  (e.g. 1.4) and confirm the rendered spot shows the expected Airy-like
  central lobe/ring structure rather than a bare Gaussian, that switching
  to `GibsonLanni` also works, and that total photon counts/brightness
  look consistent with `Gaussian` mode at the same NA/wavelength.

## Step 2 — 3D vectorial PSF with random per-emitter Z spread

- Extend `PsfGeneratorRequest`/config building to request a real z-stack
  (nz > 1, resAxial = z-step) from the same bridge call. Add two new
  **user-facing MM properties** for this rather than hardcoding a range:
  `PsfZRangeUm` (total z-stack span, e.g. default ±1 µm) and `PsfZStepUm`
  (z-plane spacing, e.g. default 0.05-0.1 µm), both wired through
  `InvalidateStack()` like every other PSF-affecting property.
- Add `BlinkEvent.zUm` (new field in `Simulation/SMLMSimulation.h`), drawn
  from a Gaussian at emitter-spawn time (both `GenerateAllEvents` and
  `AdvanceOneFrame`) using a new property `PsfZSpreadStdNm` (0 = disabled,
  matching the `driftNmPerSecX = 0` disabled-by-default convention already
  used).
- `SplatPsfKernel` picks the nearest cached z-plane (or linearly
  interpolates the two nearest) to `e.zUm`.
- **Test**: with a vectorial `PsfModel` and `PsfZSpreadStdNm > 0` in Live
  mode, confirm individual spots show a distribution of focus states
  (sharp near Z=0, ring-spread further out) rather than uniform sharpness,
  and that changing `PsfZRangeUm`/`PsfZStepUm` visibly changes the cached
  stack's coverage/resolution.

## Step 3 — Revert random Z spread; add a real Z-stage device

- Stop drawing `BlinkEvent.zUm` randomly; all emitters share one global
  focus offset instead.
- New shared-state header (e.g. `Simulation/SharedStageState.h`): a small
  struct with `std::atomic<double> zPositionUm`, reachable from both device
  instances (simplest: a process-wide singleton — no MM device-linking
  needed).
- New device `SMLMDemoZStage` (new `.h`/`.cpp` under
  `DeviceAdapter/SMLMDemoCam/`, subclassing `CStageBase<SMLMDemoZStage>`),
  implementing `SetPositionUm`/`GetPositionUm`/`SetPositionSteps`/
  `GetPositionSteps`/`Home`/`Stop`, writing to the shared `zPositionUm`.
  Registered in the module's device registration alongside the existing
  camera registration so it appears in the Hardware Configuration Wizard.
- `CSMLMDemoCamera` reads the shared `zPositionUm` each frame (both
  `StackGenerationWorker`'s per-frame loop and `LiveProducerLoop`) as a
  uniform Z offset applied to every emitter when selecting/interpolating
  the cached z-plane in `SplatPsfKernel`.
- **Test**: add "SMLMDemoZStage" via the Hardware Configuration Wizard,
  move it via MM's Stage Control panel, confirm Live mode PSFs
  sharpen/blur in sync in real time.

## Step 4 — Compare against the SMLM Challenge (Sage et al. 2019) methodology

Research-only step (no code changes unless you approve adopting a specific
finding as a follow-up), done here — before adding Zernike aberrations —
so its findings can inform whether/how the Zernike step and its defaults
should be shaped. Compares this plugin's PSF *and* noise models against
D. Sage et al., "Super-Resolution Fight Club: Assessment of 2D and 3D
Single-Molecule Localization Microscopy Software," *Nature Methods* 16(5),
387-395, 2019 — the ground-truth simulator behind that community benchmark.

- Obtain the paper's Methods/Online Methods + Supplementary Information
  describing how the benchmark's simulated datasets (2D, astigmatic 3D,
  biplane 3D, double-helix 3D) were generated: which PSF model/tool was
  used per modality (including whether it used PSFGenerator itself, an
  aberrated/astigmatic vectorial model, or something else), and the camera
  noise chain (EMCCD vs. sCMOS handling). The paper is paywalled at Nature
  Methods — note you may need to supply the PDF/supplement, or a
  preprint/author-page copy will be searched for.
- PSF comparison: their model/parameters/aberration handling per modality
  vs. this plugin's `PsfModel` choice (Richards-Wolf/Gibson-Lanni,
  oversampling factor) from steps 1-3, and whether their 3D modalities
  (astigmatic, biplane, double-helix) imply aberration types worth
  prioritizing once the Zernike step is built.
- Noise comparison: this plugin's current chain in
  [`SMLMNoise.cpp`](DeviceAdapter/SMLMDemoCam/Simulation/SMLMNoise.cpp) —
  Poisson shot noise → scalar Gaussian read noise → scalar gain → static
  per-pixel offset map, with **no EM-gain excess-noise (gamma) term and no
  per-pixel sCMOS gain/read-noise maps** — vs. what the paper's simulator
  used for its EMCCD/sCMOS ground truth.
- Deliverable: a written comparison document listing concrete, cited
  differences (not a code diff), with a recommendation on which (if any)
  are worth adopting as a separate follow-up — including, specifically,
  which Zernike modes/magnitudes would be worth defaulting to in the next
  step to match realistic astigmatic/biplane/DH-PSF 3D conditions.

## Step 5 — Static Zernike aberrations (patched PSFGenerator build)

- Fork/clone `Biomedical-Imaging-Group/PSFGenerator` locally; add a new PSF
  model class (e.g. `src/psf/richardswolf/RichardsWolfZernikePSF.java`)
  implementing a full 2D pupil-plane Richards-Wolf integral over
  `(rho, phi)` — not the existing radial-only method — with an added pupil
  phase term `2*pi * sum_j c_j * Z_j(rho, phi)` (standard OSA/ANSI
  single-index Zernike basis). Port the algorithm from the Python
  `psf_generator` package's `VectorialSphericalPropagator` (MIT-licensed
  reference implementation — port the math/algorithm into fresh Java, not
  copy Python source text). Register it in `CollectionPSF` alongside the
  existing `RichardsWolfPSF`/`GibsonLanniPSF` models, with a new config key
  for Zernike coefficients (comma-separated `index:value` pairs), and add
  it to `PsfBridgeMain.java`'s model dispatch.
- Build the patched jar with the repo's existing Ant `build.xml`. Document
  clearly (in code comments and to you) that this step onward requires this
  custom-built jar, not the stock EPFL download — this is the "necessary
  plugin that needs to be downloaded" for this feature specifically, and as
  a locally-built GPL-3.0 derivative work used privately it's licensing-safe
  (no redistribution); the bridge/subprocess boundary keeps it out of this
  project's own BSD-licensed C++ codebase.
- On the C++ side, add `ZernikeCoefficients` string property
  (comma-separated, OSA index:value), parsed/formatted with a helper
  mirroring `ParseResolutionSpacingsNm`/`FormatResolutionSpacingsNm`
  (`Simulation/SMLMPatterns.h/.cpp`) — implemented in
  `PsfGeneratorBridge.h/.cpp`. Feeds into the config file the same way as
  every other PSF param; recompute-on-change already falls out of the
  existing `InvalidateStack()` pattern, satisfying "after these are
  changed, re-create the oversampled PSF once and sample from that" with no
  new plumbing.
- **Test**: with `PsfModel = RichardsWolf`, set e.g. index 6 (astigmatism)
  or 7/8 (coma) to a nonzero value in Live mode and confirm the rendered
  PSF shape visibly distorts (elongates/becomes asymmetric); confirm it
  reverts to matching steps 1-3's unaberrated output when reset to all
  zeros (regression check against the un-patched model).

## Verification (all steps)

Build the device adapter (`DeviceAdapter/SMLMDemoCam/SMLMDemoCam.sln`/
`.vcxproj`), load it into Micro-Manager, and visually inspect Live mode
frames after each step as described above. No existing automated test
suite was found for this project — confirm with a build + Micro-Manager
Live-mode check at each stage before moving to the next. Step 5
additionally needs `ant` (or manual `javac`) to build the patched
`PSFGenerator.jar`, and a Java runtime available at `PsfGeneratorJavaPath`
for every step from 1 onward.
