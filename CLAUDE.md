# demoCam_SMLM_MM

A synthetic SMLM (Single-Molecule Localization Microscopy) camera device
adapter for Micro-Manager. Generates blinking-fluorophore movies with
realistic PSFs and camera noise, either as a reproducible precomputed stack
or a live-streaming mode with parameters adjustable while running.

- Device adapter source: `DeviceAdapter/SMLMDemoCam/`
- Core simulation engine (no MMDevice dependency): `DeviceAdapter/SMLMDemoCam/Simulation/`
- Build: `DeviceAdapter/SMLMDemoCam/SMLMDemoCam.sln` (MSBuild, Release|x64),
  outputs `mmgr_dal_SMLMDemoCam.dll` to `DeviceAdapter/SMLMDemoCam/build/Release/x64/`

## MM property naming convention

Every user-facing MM property name is prefixed with the group it belongs
to, mirroring the UI section groupings in the webSMLM reference simulator
(`C:\GitHub\websmlm\webSMLM.html` -- see that project's `PARITY.md`):

- `General_` -- FOV/binning/acquisition-mode/stack-playback plumbing, plus
  every property that sat in webSMLM's flat "User parameters" group
  (density, pixel size, labeling efficiency, frame-interval readback).
  Includes MM-adapter-only properties with no webSMLM equivalent at all
  (`AcqMode`, `GenerateStack`, `UseGpu`, `GpuStatus`, etc.).
- `SimType_` -- webSMLM's "Simulation type" group: `Pattern` and every
  structure/pattern-shape parameter (`CustomPointsFile`,
  `ResolutionSpacingsNm`, `StructureZRangeNm`, `StructureSizeNm`, all
  `Nup*`), plus `DriftNmPerSec` and `RandomSeed`.
- `FluoParam_` -- webSMLM's "Fluorophore parameters" group:
  `PhotonsPerSecond`, `OnLifetimeSec`, `BlinkBleachProb`, `OffLifetimeSec`,
  `PhotonCV`, `IllumProfile`, `IllumFwhmPct` (webSMLM puts its
  illumination profile in this group too).
- `CamParam_` -- webSMLM's "Camera parameters" group: gain, offset,
  offset-std, read noise, QE, dark current, the sCMOS per-pixel-map
  std-pct properties, and the EMCCD ones (`CameraType`, `EmGain`,
  `CicElectrons`, `BitDepth`).
- `PSFParam_` -- webSMLM's "PSF parameters" group: every `Psf*` property
  (`PsfModel`, `PsfNa`, `PsfEmissionWavelengthNm`, `PsfInterp`,
  `PsfMaskType`/`PsfMaskModes`/`PsfMaskWaist`, etc., including
  `PsfGeneratorJavaHome`, which has no direct webSMLM analog but is
  PSF-generator-specific machinery).
- `Background_` -- webSMLM's "Background" group (added in its 2026-09-19
  builds): `BackgroundPhotonsPerSec` (was `General_BackgroundPhotonsPerSec`),
  `CellContrast`, `HazeWeight`, `HazeWidthNm`, `DecaySec`,
  `OutOfFocusRatio`, `OutOfFocusDepthNm`.

Standard MM keywords this device inherits (`Exposure`, `PixelType`,
`Name`, `Description`, `CameraName`, `CameraID`, and `SMLMDemoZStage`'s
`Position`) are **not** prefixed -- MM Core and Micro-Manager Studio's own
GUI depend on those literal names, and they aren't part of this project's
own property surface. Allowed-*value* strings (e.g. `Circle`, `Gaussian`,
`TopDown`, `Direct`) are also unprefixed -- only property *names* get a
group prefix.

**Keep this convention up to date**: any new MM property added to this
device must get one of the six prefixes above (pick by which webSMLM UI
section the analogous concept would sit in, or `General_` if there's no
webSMLM analog at all) -- update this section's bullet list and, if the
mapping to webSMLM's groups shifts, `PARITY.md` in the websmlm repo too.

## Vectorial PSF feature -- status

Full plan: [docs/vectorial-psf-plan.md](docs/vectorial-psf-plan.md).

**Step 1 (in-focus 2D vectorial PSF) is done and working**, as of commit
`a4e5f28`. **Steps 2 (Z-stack) and 3 (real `SMLMDemoZStage` device) are
both done and visually confirmed working in Micro-Manager.** **Step 4
(SMLM Challenge comparison, research-only) is done** -- see
[docs/vectorial-psf-step4-smlm-challenge-comparison.md](docs/vectorial-psf-step4-smlm-challenge-comparison.md);
key takeaway: the Challenge's ground truth is a measured PSF lookup table,
not a parametric/Zernike model, so it supplies no numeric Zernike targets
for step 5, and biplane/double-helix are out of scope for a single-pupil
Zernike patch regardless. **Step 5 (Gibson-Lanni + Zernike aberrations) is
code-complete and builds clean end-to-end (Java smoke test + full MSBuild
solution build); not yet visually verified inside Micro-Manager itself.**
Retargeted from `RichardsWolf` to `GibsonLanni` during implementation, and
built as a new all-Java class (no PSFGenerator fork/Ant needed after all) --
see `docs/vectorial-psf-plan.md`'s Step 5 section for the full story
(including a correction to that doc's own earlier "index 6 = astigmatism"
claim -- it's actually index 5).

### Architecture

The Gaussian PSF renderer (`Simulation/SMLMSimulation.cpp`,
`RenderGaussianPSF`) is still there and still the default fallback. The
vectorial renderer computes one oversampled PSF kernel per parameter change
(cached) and downsamples+splats it at every emitter position per frame --
"oversample once, downsample everywhere" (`Simulation/PsfGeneratorBridge.h/.cpp`,
`SplatPsfKernel`).

The PSF itself is computed by EPFL Biomedical Imaging Group's
[PSFGenerator](https://github.com/Biomedical-Imaging-Group/PSFGenerator)
(GPL-3.0, Richards-Wolf + Gibson-Lanni models), **embedded directly into
the DLL**:

- `Simulation/psfbridge-java/psfbridge/PsfBridge.java` is this project's
  own small driver class (NOT part of PSFGenerator) -- calls PSFGenerator's
  PSF model classes directly and returns a raw `float[]` of computed
  intensity planes.
- `Simulation/psfbridge-java/psfbridge/GibsonLanniZernikePSF.java` is
  likewise this project's own class (step 5), also NOT part of
  PSFGenerator -- it just extends PSFGenerator's public `psf.PSF` base
  class the same way `PsfBridge.java` instantiates PSFGenerator's own
  models, so no PSFGenerator source fork was needed for this feature after
  all (see its class Javadoc and the "Building" section below).
- That class is compiled and merged into a copy of a downloaded
  `PSFGenerator.jar`, producing `third_party/SMLMPsfEmbedded.jar` (**not
  committed** -- GPL, built locally, see "Building" below).
- That merged jar is embedded as a Win32 resource (`SMLMDemoCam.rc`,
  `IDR_PSF_JAR`) directly into `mmgr_dal_SMLMDemoCam.dll`.
- At runtime, `PsfGeneratorBridge.cpp` extracts that resource to a temp
  file once and loads it into an **in-process JVM via the JNI Invocation
  API** -- no external `java.exe` subprocess, no separate bridge-jar file
  to deploy or configure.

**Licensing consequence (deliberate, user-approved tradeoff):** because
PSFGenerator's GPL-3.0 bytecode is linked into the DLL, the *built*
`mmgr_dal_SMLMDemoCam.dll` is a combined work under GPL-3.0, distinct from
the rest of this project's BSD license. See `DeviceAdapter/SMLMDemoCam/license.txt`
and the header comment in `PsfBridge.java`.

### Building (vectorial PSF feature)

1. Download `PSFGenerator.jar` from https://bigwww.epfl.ch/algorithms/psfgenerator/
   -- specifically the **"Java application for standalone/Matlab"** download
   (self-contained bundle). The "Plugin for Icy/ImageJ/Fiji" variant
   (`PSF_Generator.jar`) is missing dependencies this bridge needs and will
   not work.
2. Place it at `third_party/PSFGenerator.jar` (gitignored).
3. Compile the bridge **targeting Java 8 bytecode** (see "Gotchas" below
   for why this matters) -- this now compiles both of this project's own
   classes, `PsfBridge.java` and `GibsonLanniZernikePSF.java` (step 5):
   ```
   cd DeviceAdapter/SMLMDemoCam/Simulation/psfbridge-java
   javac --release 8 -cp ../../../../third_party/PSFGenerator.jar -d out psfbridge/PsfBridge.java psfbridge/GibsonLanniZernikePSF.java
   ```
4. Merge the compiled classes into a copy of PSFGenerator.jar (note the
   nested-class `$PlaneJob` entry -- `GibsonLanniZernikePSF`'s per-Z-plane
   worker):
   ```
   cp third_party/PSFGenerator.jar third_party/SMLMPsfEmbedded.jar
   cd DeviceAdapter/SMLMDemoCam/Simulation/psfbridge-java/out
   jar uf ../../../../../third_party/SMLMPsfEmbedded.jar psfbridge/PsfBridge.class psfbridge/GibsonLanniZernikePSF.class "psfbridge/GibsonLanniZernikePSF\$PlaneJob.class"
   ```
5. Build the solution (MSBuild picks up `third_party/SMLMPsfEmbedded.jar`
   via `SMLMDemoCam.rc` and embeds it into the DLL automatically):
   ```
   MSBuild.exe SMLMDemoCam.sln /p:Configuration=Release /p:Platform=x64
   ```

Whenever `psfbridge/PsfBridge.java` or `GibsonLanniZernikePSF.java` change,
repeat steps 3-5 (the merged jar must be rebuilt and the DLL relinked --
MSBuild's resource compiler only re-embeds when the `.jar` file's timestamp
changes, so a plain re-`jar uf` with the same filename is enough to trigger
it; no `ant`/PSFGenerator-fork build step exists or is needed, unlike what
the original step 5 plan anticipated -- see docs/vectorial-psf-plan.md).

A JRE/JDK must be present at runtime too (to supply `jvm.dll`) -- see
`PSFParam_PsfGeneratorJavaHome` property below.

See the "webSMLM parity feature" section below for the 3D-structure/
labeling-efficiency/`PSFParam_PsfInterp` properties (and the since-removed `PSFParam_PsfEvalMethod`) added on top of
everything in this section.

### New MM properties

- `PSFParam_PsfModel` -- `Gaussian` | `RichardsWolf` | `GibsonLanni` |
  `GibsonLanniZernike` (default: `GibsonLanniZernike`)
- `PSFParam_PsfImmersionIndex`, `PSFParam_PsfOversampling` (default 6; was 12
  before step 5, then 4 -- kept small so the out-of-the-box oversampled
  kernel stays manageable regardless of `PSFParam_PsfModel`, see the step 5
  performance Gotcha), `PSFParam_PsfKernelHalfWidthNm` (default 7000 nm).
  That last one is in NANOMETERS, not camera pixels (it was
  `PSFParam_PsfKernelHalfWidthPx`, default 16 px, until this rename):
  `BuildPsfGeneratorRequest()` rounds it to a whole pixel count against the
  current `General_PixelSizeNm`, so the rendered window covers a fixed
  physical extent whatever the pixel size is. It is still only a *minimum*,
  auto-grown from NA/wavelength/pixel size -- see Gotchas.
- `PSFParam_PsfGeneratorJavaHome` -- optional JRE/JDK root override; auto-detects
  otherwise (`JAVA_HOME`, then common Windows install paths)
- `PSFParam_PsfZRangeUm` (default 7.0, max 20.0), `PSFParam_PsfZStepUm` (default 0.1) -- Z-stack range/
  step (step 2). The per-emitter random Z spread step 2 originally added
  here (`PsfZSpreadStdNm`) was removed again in step 3, replaced by a real
  `SMLMDemoZStage` device (`MM::Stage`) driving one global, user-drivable
  focus offset -- see `Simulation/SharedStageState.h`. Add both
  "SMLMDemoCam" and "SMLMDemoZStage" via the Hardware Configuration Wizard;
  they communicate through a process-wide singleton, no explicit MM
  device-linking needed.
- `PSFParam_PsfSampleIndex`, `PSFParam_PsfWorkingDistanceUm` (default 150.0),
  `PSFParam_PsfSampleDepthNm` -- GibsonLanni-only PSFGenerator parameters, ignored
  by RichardsWolf; `PSFParam_PsfSampleIndex` defaults to matching
  `PSFParam_PsfImmersionIndex` and `PSFParam_PsfSampleDepthNm` defaults to 0, reproducing
  PsfBridge.java's old hardcoded no-mismatch/in-focus behavior rather than
  PSFGenerator's own stock defaults (1.33 / 2000)
- `PSFParam_PsfZernikeCoefficients` (string, 28 space-separated values;
  a 15-value list is still accepted and zero-padded; defaults to the
  `MixedRealisticObjective` preset's values, NOT all-zero) --
  `GibsonLanniZernike`-only; positional OSA/ANSI single Zernike index 0-27
  (n <= 6), in waves. See `Simulation/SMLMZernike.h`'s
  `ZernikeCoefficients` doc comment for the exact index-to-mode mapping (0
  piston ... 5 vertical astigmatism ... 7/8 coma ... 12 primary spherical
  ... 25 vertical tertiary astigmatism). **Space-separated because MMCore
  rejects a comma in any property value set through it**
  (`MM::g_FieldDelimiters`) -- the old comma-separated format could never
  actually be set from Studio/pymmcore; same fix for
  `SimType_ResolutionSpacingsNm`. Parsers accept spaces, commas or
  semicolons; the JVM bridge still gets a comma-separated copy.
- `PSFParam_PsfZernikePreset` -- convenience dropdown on top of
  `PSFParam_PsfZernikeCoefficients`: `None` | `AstigmatismWeak` |
  `AstigmatismModerate` | `AstigmatismStrong` | `ComaWeak` | `ComaStrong` |
  `SphericalWeak` | `SphericalStrong` | `TrefoilModerate` |
  `MixedRealisticObjective` (the default) | `SaddlePoint` | `ExtendedRange` |
  `ExtendedRangeStrong`. Selecting one overwrites
  `PSFParam_PsfZernikeCoefficients`. **Values are webSMLM's own
  `PSF_ZERNIKE_PRESETS`, one-to-one** (realigned in parity round 2 -- e.g.
  coma moved from index 8 to 7, trefoil from 9 to 6); the single-mode ones
  are order-of-magnitude wavefront-error estimates (~0.07-0.15 waves =
  mild, ~0.2-0.3 waves = strong), **not numbers sourced from a specific
  paper**; the last three are engineered stacked-astigmatism PSFs for a
  longer z range. See `Simulation/SMLMZernike.cpp`'s
  `ZernikePresetCoefficients` for the exact values. Editing
  `PSFParam_PsfZernikeCoefficients` directly afterwards does not update/clear this
  property -- it only ever reports the last preset explicitly selected
  through it.

### Noise model properties (Kinetix22 sCMOS defaults)

`Simulation/SMLMNoise.h/.cpp`'s chain: QE -> + dark current -> Poisson shot
noise -> (per-pixel or scalar) Gaussian read noise -> (per-pixel or scalar)
gain -> static per-pixel additive offset -> 16-bit clamp. New properties
`CamParam_QuantumEfficiency`, `CamParam_DarkCurrentElectronsPerSec`, `CamParam_GainStdPctPerPixel`,
`CamParam_ReadNoiseStdPctPerPixel` (the last two are relative pixel-to-pixel spread on
top of `CamParam_GainPhotonsPerADU`/`CamParam_ReadNoiseElectrons`, sCMOS-style;
`0` = every pixel identical, matching the `DriftNmPerSec = 0`
disabled-by-default convention). Defaults for `CamParam_QuantumEfficiency`
(0.85), `CamParam_DarkCurrentElectronsPerSec` (1.03), `CamParam_GainPhotonsPerADU`
(0.25), and `CamParam_ReadNoiseElectrons` (1.2) match the Photometrics Kinetix22
sCMOS Sensitivity (CMS) mode datasheet; `CamParam_GainStdPctPerPixel`/
`CamParam_ReadNoiseStdPctPerPixel` default to 5%/20% as estimates, since Photometrics
doesn't publish actual per-pixel variance. See
[docs/vectorial-psf-plan.md](docs/vectorial-psf-plan.md)'s "Noise model
follow-ups" section for sourcing and what's still deprioritized (PRNU,
background vignetting, full-well/bit-depth). **Not yet visually verified
in Micro-Manager.**

### Gotchas found during step 1 (don't rediscover these)

- **`PSF.process()` hangs.** PSFGenerator's own `Pool.waitTermination()`
  busy-spins on a non-volatile flag with no synchronization -- a real bug
  in PSFGenerator's threading library, reproducible via JVM thread dump.
  Fix: `PsfBridge.computePlanes()` replicates `PSF.process()`'s steps
  manually and drives execution with `ExecutionMode.MULTITHREAD_NO`
  instead (synchronous, no thread pool) -- same code path PSFGenerator's
  own `PSFGenerator.compute()`/`computeImagePlus()` use.
- **Second JVM crashes the host process.** Classic Micro-Manager
  (`MMStudio`) is itself a Java app already running a JVM in-process
  before our DLL loads. `JNI_CreateJavaVM` does not support a second JVM
  per process -- HotSpot can hard-crash the whole process with no
  catchable exception. Fix: `EnsureJvmCreated` in `PsfGeneratorBridge.cpp`
  checks for an already-loaded `jvm.dll` via `GetModuleHandleA` and
  attaches to the existing JVM via `JNI_GetCreatedJavaVMs` instead of
  creating a new one. Since that means our jar isn't on the host's
  classpath, class loading goes through an explicit `URLClassLoader`
  (`ResolveBridgeClass`), not plain `FindClass`.
- **`UnsupportedClassVersionError` under MMStudio's bundled JRE.** A plain
  `javac` defaults to the *compiling* JDK's newer class file version.
  Compile with `--release 8` explicitly (see Building above).
- **`GibsonLanniPSF`'s own defaults model a deliberately aberrated
  scenario** (sample index 1.33 vs. a typical oil immersion index ~1.5+,
  particle 2µm deep) -- not a fair comparison to `RichardsWolfPSF`'s
  always-in-focus, no-mismatch model. `PsfBridge.java` now explicitly
  matches sample index to immersion index and zeroes the depth offset for
  GibsonLanni.
- **The first-order Airy ring is very faint** (~0.1-2% of peak) --
  physically correct, not a bug. It's easy to mistake for "not rendering"
  when it's actually just below the noise floor / display contrast at
  normal photon counts. Cranking `FluoParam_PhotonsPerSecond` up does *not* help
  reveal it (makes the peak-to-ring dynamic range worse on a linear
  display); a log/gamma LUT does.
- **`PSFParam_PsfKernelHalfWidthNm` is a minimum, not the actual value used** --
  `BuildPsfGeneratorRequest()` first rounds it from nm to a whole camera-pixel
  count against the current `General_PixelSizeNm`, then auto-grows that to 3x the Rayleigh radius
  (`0.61*lambda/NA`) so the rendered window can't truncate the first ring
  regardless of NA/wavelength/pixel size, capped at 48px.
- **`CStageBase<U>` does not default-implement `IsStageSequenceable`**
  (only `IsStageLinearSequenceable`) -- omitting an override leaves the
  device class abstract and fails to compile/instantiate with `C2259`
  ("cannot instantiate abstract class") at the `new SMLMDemoZStage()` call
  site in the module's `CreateDevice`, not in `SMLMDemoZStage.h/.cpp`
  itself, which can make the error confusing to trace back.

### Gotchas found during step 5 (don't rediscover these)

**Update (parity round 2): the `Direct` evaluator and its
`PSFParam_PsfEvalMethod` property were removed** -- 40 azimuthal samples
alias beyond ~1.6 um, so on the default 6 um kernel every emitter's core
came out 17-20% too dim (webSMLM found this and removed its own copy in
build 2026-09-21e). Chirp-Z is the only evaluator now. The notes below that
discuss `Direct`/N_RHO/N_PHI performance are history.

- **The "obvious" reference algorithm (`psf_generator`'s
  `VectorialSphericalPropagator`) turns out to explicitly reject the exact
  feature being ported.** Its `SphericalPropagator` base class assumes an
  axisymmetric pupil and its Zernike helper (`utils/zernike.py`,
  `create_zernike_aberrations`, `mesh_type='spherical'`) warns and silently
  *drops* any Zernike term with `l != 0` (i.e. everything except piston/
  defocus/spherical) -- astigmatism, coma, trefoil are all `l != 0`. Only
  `VectorialCartesianPropagator` (full 2D `(kx, ky)` pupil + FFT) supports
  them. Always check whether a "vectorial"/"full 2D" reference
  implementation's *axisymmetric* sibling class quietly narrows what it
  actually supports before assuming its docstring's scope applies.
- **This model is meaningfully slower to (re)compute than the radially-
  symmetric models.** A non-axisymmetric pupil (any nonzero non-`l=0`
  Zernike coefficient) cannot be reduced to a 1D radial lookup the way
  `GibsonLanniPSF`/`RichardsWolfPSF` are, so `GibsonLanniZernikePSF` does a
  direct 2D `(rho, phi)` numerical quadrature (fixed 20x40 grid -- reduced
  from an initial 32x64, see the class's own comment on why that's safe
  accuracy-wise) per output pixel, per Z-plane -- cost scales with
  oversampled-kernel-pixels x 800 x Z-planes, and **at default-ish settings
  this is enormous**: `PSFParam_PsfZRangeUm`/`PSFParam_PsfZStepUm` defaults alone give 70+ Z
  planes, and `PSFParam_PsfOversampling`/`PSFParam_PsfKernelHalfWidthNm` can put the
  oversampled window in the hundreds of pixels per side -- a real user
  report hit a 769x769px x 71-plane kernel that was still running after
  90+ seconds serially. Two mitigations exist; **reducing the window/plane
  count is the one that actually matters** (it's a squared/linear-times
  effect, dwarfing anything the other one buys):
  - `PsfBridge.runPoolParallel` runs the (independent) per-Z-plane jobs
    across a `java.util.concurrent` thread pool sized to
    `Runtime.availableProcessors()`, instead of the stock models'
    `runPool`/`Pool.execute(MULTITHREAD_NO)` (fully serial -- deliberately
    serial for the stock models, to sidestep a separate documented
    PSFGenerator `Pool.execute(MULTITHREAD_SYNCHRONIZED)` hang bug; that
    bug lives in `Pool`'s own execution/wait machinery, which this method
    never touches, so it's not at risk here). **Gotcha hit while adding
    this**: `Job#live` (checked by every model's `process()`, e.g.
    `PlaneJob`'s `if (!live) return;`) defaults to `false` and is normally
    flipped `true` by `Pool.execute()` itself before running each job --
    calling a job's `Runnable#run()` directly without also calling its
    `init()` first silently no-ops every plane (confirmed: an early version
    of this produced an all-zero result for literally 100% of planes, with
    no exception -- easy to mis-diagnose as a physics bug rather than a
    missing lifecycle call). Fixed by explicitly calling each job's
    `init()` via reflection before submitting it.
  - `ComputePsfKernelCache` takes an optional `logCallback` (both
    `SMLMImageGeneration.cpp` call sites wire it to `LogMessage`) that logs
    a start message, a ~2s heartbeat ("still computing... Xs elapsed") for
    as long as the blocking JNI call runs, and a completion message with
    total elapsed time -- since no incremental progress crosses the JNI
    boundary during the call itself, this is purely corelog reassurance,
    not a speedup. For `GibsonLanniZernike` specifically it also logs an
    up-front warning (with concrete advice: lower `PSFParam_PsfOversampling`/
    `PSFParam_PsfKernelHalfWidthNm`/`PSFParam_PsfZRangeUm`/`PSFParam_PsfZStepUm`) whenever
    `size^2 * nz` exceeds ~20M -- see `PsfGeneratorBridge.cpp`'s
    `kSizeNzWarnThreshold`.
  - `PlaneJob.process()`'s pupil arrays (`pupilRe`/`pupilIm`) are flattened
    `double[N_RHO*N_PHI]` (one bounds-checked dereference, contiguous
    layout) rather than `double[N_RHO][N_PHI]` (two dereferences, N_RHO
    separate row objects), and the output pixel loop is nested `y` outer /
    `x` inner so `slice[x + nx*y]` is written sequentially instead of with
    stride `nx` -- both changes are pure reordering/layout, not a
    numerical change (confirmed: the zero-Zernike-vs-`GibsonLanni`
    regression check's relative L2 diff is bit-for-bit identical,
    0.1274%, before and after). Measured ~1.8-2x faster at 65x65px x 24
    planes (1.0s -> ~0.55s) from this alone; noisier/less conclusive at
    129x129px (thread-pool contention on this dev machine dominates the
    signal at that size).
  - `PSFParam_PsfOversampling`/kernel half-width defaults were lowered to
    4/16 px (from 12/32 px) at step 5 specifically because of this model's
    cost profile. This is the biggest lever of all: kernel pixel count is
    `O((kernelHalfWidthPx*oversampling)^2)`, so 12/32 -> 4/16 alone is
    roughly a 28x reduction in oversampled-kernel pixel count before any
    algorithmic change. **Both have since moved again** -- oversampling is
    now 6, and the half-width property is now
    `PSFParam_PsfKernelHalfWidthNm` at 3000 nm (= 30 px at the default 100
    nm pixel size), i.e. roughly 5x the step-5 pixel count. That is
    affordable only because the chirp-Z evaluator is used (now the only
    one -- `Direct` was removed in parity round 2).
    See the property list above.
  - Benchmarked (this dev machine, 12 logical cores, post all of the above):
    65x65px x 24 planes ~0.5-0.6s; 129x129px x 24 planes ~2.4-3.1s. Both
    scale roughly with `size^2 * nz`, so a user-chosen 769x769 x 71
    combination (achievable by raising `PSFParam_PsfOversampling`/
    `PSFParam_PsfKernelHalfWidthNm`/`PSFParam_PsfZRangeUm` well past their new defaults)
    would still land in the multi-minute range even fully parallelized --
    there is no substitute for keeping the window/plane count down for
    this model specifically; the corelog warning above exists for exactly
    this case.
  - **Not attempted, and deliberately not attempted**: a Jacobi-Anger/
    Bessel-series reformulation (precompute each pupil ring's discrete
    Fourier series over `phi`, then reconstruct per-pixel via Bessel
    functions instead of a raw `(rho,phi)` double sum) would cut the
    per-pixel inner-loop trig-call count substantially, the same way the
    *radially-symmetric* models already avoid it entirely (their whole
    speed advantage IS this trick, in the one case it applies exactly --
    see `KirchhoffDiffractionSimpson`). It doesn't apply for free once the
    pupil itself is `phi`-dependent (any non-axisymmetric Zernike term),
    but the phi-Fourier-decomposition version above still works — at the
    cost of implementing a solid Bessel-function evaluator and a real risk
    of subtle correctness bugs that are hard to catch without a reference
    implementation to diff against. Given this session's realistic ability
    to validate such a rewrite, the defaults change + the safe layout/
    ordering wins above were judged the better cost/risk trade for now;
    revisit if `GibsonLanniZernike` performance becomes a blocker again
    even at sane window/Z-range settings.
- **The plan's own illustrative "index 6 = vertical astigmatism" was
  wrong.** Working the actual OSA/ANSI single-index formula
  (`j = n(n+1)/2 + l`) gives vertical astigmatism at index **5**, not 6 --
  see `Simulation/SMLMZernike.h`'s doc comment for the verified full 0-14
  mapping. Trust that comment over any earlier planning-doc prose.
- **No JDK/`ant` was actually needed for this step**, contrary to the
  original plan's assumption that Zernike support required forking and
  building PSFGenerator's own source tree. `PsfBridge.java` already
  bypasses PSFGenerator's `CollectionPSF`/Settings/GUI registration layer
  and instantiates PSF model classes directly -- so a new model is just
  another class extending the public `psf.PSF` API, compiled the exact
  same way `PsfBridge.java` itself already is. Worth checking whether an
  existing bypass/adapter layer already sidesteps a piece of "the real
  library's" architecture before assuming a fork is required to extend it.

### Next steps

See `docs/vectorial-psf-plan.md` for the full write-up. In order:

1. ~~Step 1: in-focus 2D vectorial PSF~~ -- done
2. ~~Step 2: real Z-stack (`nz > 1`) + random per-emitter Z spread~~ --
   code-complete, not yet visually verified
3. ~~Step 3: revert random Z spread; add a real `MM::Stage` device
   (`SMLMDemoZStage`) for a global, user-drivable focus offset~~ --
   code-complete, not yet visually verified
4. ~~Step 4: compare this plugin's PSF + noise models against the Sage et
   al. 2019 "Super-Resolution Fight Club" SMLM Challenge methodology~~ --
   done (research-only)
5. ~~Step 5: Gibson-Lanni + Zernike aberrations~~ -- code-complete (Java
   smoke test + full solution build both pass); **not yet visually
   verified inside Micro-Manager** -- that visual check (load the DLL, set
   `PsfModel = GibsonLanniZernike`, try a `PSFParam_PsfZernikePreset`, confirm it
   looks like existing `GibsonLanni` at all-zero and visibly aberrates
   otherwise) is the one remaining piece of this feature.

## webSMLM parity feature -- status

Brings this plugin's simulation engine closer to feature parity with the
reference simulator at `C:\GitHub\websmlm` (`webSMLM.html`) -- see that
project's `PARITY.md` for the full parameter/feature correspondence table
this work was scoped against. Direction is **MM-only**: websmlm itself was
not modified beyond that one spec file. **Code-complete and verified via
`tools/test_smlmcam.py` (extended with new assertions) + `MSBuild` builds
at every step; not yet visually confirmed inside Micro-Manager Studio's own
GUI** (only via pymmcore-plus/headless `CMMCorePlus`, which exercises every
property and the full render pipeline but isn't the same as a human looking
at the rendered image in the actual app).

**Per-emitter 3D + 3D/NPC structures + labeling efficiency** -- done.
`EmitterSite`/`BlinkEvent` gained `zNm`; the vectorial-PSF plane lookup in
`RenderPhotonImage` now happens **per emitter** (stage offset + emitter's
own depth add), not once per frame. New `Simulation/SMLMStructures.h/.cpp`:
`SiteListPattern` (a finite pre-generated site list, needed because
labeling efficiency and NUP's per-site linker displacement both require a
site to persist across the whole movie -- see that file's header comment
for why this is a deliberate, documented exception to `SMLMPatterns.h`'s
otherwise strictly-continuous-sampling design) and builders for
`TiltedPlane`/`Uniform3D`/`Shell`/`NUP` (Thevathasan et al. 2019 Nup96 NPC
geometry, parametrized per Wanninger et al. 2023 "CIR4MICS": 2 rings x 8
corners x 4 arc points, uniform-in-volume linker displacement, rejection-
sampled center spacing, mean-subtracted bowl curvature, TopDown/Sideways
membrane orientation). `ZSpreadPattern` decorator gives the 9 existing
continuous 2D patterns an optional uniform z spread. 12 new MM properties:
`General_LabelingEfficiencyPct` (default 70), `SimType_StructureZRangeNm` (default
**500** -- the one deliberate default that changes existing seeds'
rendered output, see below), `SimType_StructureSizeNm`, and 8 `Nup*` properties.
`SimType_Pattern` gained `TiltedPlane`/`Uniform3D`/`Shell`/`NUP`. Site-list
generation and labeling use their own RNG stream (`structureSeed`),
independent of the arrival/noise stream, so this feature's *code* cannot
shift existing output -- only the `StructureZRangeNm=500` default does
that, deliberately.

**Sub-pixel PSF placement** -- done. New `PSFParam_PsfInterp` property
(`Nearest`|`Linear`|`Cubic`, default `Cubic`; `Nearest` reproduces the
original exact box-average behavior). `Linear`/`Cubic` sample the oversampled kernel plane at the
emitter's true continuous position (bilinear / Catmull-Rom bicubic) instead
of quantizing to steps of 1/`PSFParam_PsfOversampling` of a camera pixel.

**Chirp-Z (Bluestein) fast evaluator** -- done, and since parity round 2 the ONLY evaluator (`Direct` and this property were removed, see the step 5 Gotchas update). New `PSFParam_PsfEvalMethod`
property (`Direct`|`ChirpZ`, default `ChirpZ`; `Direct` reproduces the
original exact per-pixel polar-quadrature sum), `GibsonLanniZernike`-only. `ChirpZ` reformulates the
same continuous pupil-to-image integral on a Cartesian pupil grid via a
separable 2D chirp-Z transform (ported from webSMLM's `psfCzt1d`/
`computePsfPupilCartesianForZPlane`/`computePsfIntensityPlaneFFT` -- see
`GibsonLanniZernikePSF.java`'s class Javadoc "Chirp-Z evaluator" section
for the physics). Verified via a standalone Java harness (not part of the
embedded jar/DLL): `Direct` and `ChirpZ` agree to 0.22-0.29% relative L2
(after the sum-to-1 normalization `SplatPsfKernel` always applies anyway --
the two evaluators do not share the same absolute intensity scale, a fixed
~49.6x ratio, harmless for exactly that reason). Measured ~3.7x faster
(1.92s -> 0.52s) at 65x65px/9-plane/`AstigmatismModerate` settings through
the actual DLL -- directly addresses the multi-minute `GibsonLanniZernike`
recompute problem documented in the Gotchas above. Requires the same
jar-rebuild-and-relink step as any other `psfbridge-java/` change (see
"Building" above).

**Calibration bead field (`Calibration9Spots`)** -- done.
`SimType_Pattern` gained a `Calibration9Spots` value: nine ALWAYS-ON
emitters on a 3x3 grid centered in the FOV, spaced by a quarter of the
smaller FOV side. Ported from webSMLM's `generateCalibrationStack()` bead
field -- the specimen a real astigmatic-PSF z-calibration acquisition uses.
Being always-on is the point: drive `SMLMDemoZStage` through focus and
every frame shows the same nine spots at the same x,y, with only the PSF
shape changing.

Mechanically this is the first pattern that opts out of the blinking model
entirely, via a new `IPatternGenerator::AlwaysOnSites()` virtual (default
`false`, so every other pattern is untouched). When it returns `true`,
`EmitterModel::GenerateAllEvents`/`AdvanceOneFrame` skip the Poisson-arrival/
exponential-ON-lifetime process altogether and emit one full-brightness
event per site -- so `General_EmitterDensityPerSec` and
`FluoParam_OnLifetimeSec` have no effect at this pattern, and it consumes
zero rng draws. `CreatePattern` deliberately does NOT wrap it in
`ZSpreadPattern` (a random per-bead z would defeat the purpose, and that
decorator does not forward `AlwaysOnSites` anyway).

**Default-value changes and property renames** (a later pass over the
above, all deliberate, all changing what a fresh config starts at):

- Defaults now: `General_LabelingEfficiencyPct` 70, `PSFParam_PsfEvalMethod`
  `ChirpZ` (property since removed), `PSFParam_PsfInterp` `Cubic`, `PSFParam_PsfModel`
  `GibsonLanniZernike`, `PSFParam_PsfOversampling` 6,
  `PSFParam_PsfZernikePreset` `MixedRealisticObjective` (and
  `PSFParam_PsfZernikeCoefficients` correspondingly non-zero),
  `SimType_NupCount` 80, `SimType_RandomSeed` 42.
- `PSFParam_PsfKernelHalfWidthPx` -> `PSFParam_PsfKernelHalfWidthNm`
  (default 3000 nm), rounded to a whole pixel count internally -- see the
  property list above.
- `General_StackLength` and `General_StackLoop` removed. The precomputed
  stack is now a fixed 1000 frames (the old default) and always loops;
  `stackLength_`/`stackLoop_` survive as plain members so re-exposing
  either is a one-line property add. Consequence worth knowing: a
  precomputed generation pass at the new `GibsonLanniZernike` default costs
  tens of seconds and there is no longer a knob to shorten it.
- `CamParam_` renames, dropping the redundant `Camera` infix and moving
  `Pixel` to a `PerPixel` suffix: `CameraGainPhotonsPerADU` ->
  `GainPhotonsPerADU`, `CameraOffsetADU` -> `OffsetADU`,
  `CameraOffsetStdADU` -> `OffsetStdADU`, `PixelGainStdPct` ->
  `GainStdPctPerPixel`, `PixelReadNoiseStdPct` -> `ReadNoiseStdPctPerPixel`.
  The C++ `g_Prop*` identifiers and `On*` handler names were deliberately
  NOT renamed alongside them (they already didn't track the property
  strings exactly -- e.g. `OnCameraGain` for `g_PropGain`).

## webSMLM parity round 2 (2026-09-21) -- status

Catches up with webSMLM builds 2026-09-19a through 2026-09-21e (see that
repo's `PARITY.md`, refreshed on both sides in this round). Verified via:
- `tools/test_smlmcam.py` (extended);
- a bit-for-bit cross-check against webSMLM's own chirp-Z JS
  (`tools/psf_parity_check/`, now covering n<=6 Zernikes, the double-helix
  mask and the depth focal shift: 0.0000% relative L2);
- MSBuild.

**Not yet visually confirmed in Micro-Manager Studio's GUI.**

- **PSF**:
  - `Direct` evaluator removed (see the Gotchas update above).
  - 28 Zernike coefficients (OSA 0-27).
  - Presets realigned to webSMLM's values, plus `SaddlePoint`/
    `ExtendedRange`/`ExtendedRangeStrong`.
  - `PSFParam_PsfMaskType = DoubleHelix` (Gauss-Laguerre, `PsfMaskModes`
    default 5, `PsfMaskWaist` default 1.0 pupil radii; in
    `GibsonLanniZernikePSF.java`).
  - Gibson-Lanni focal shift: the z stack is centred at
    `ti0 - depth*ni/ns`, a no-op at the default `PsfSampleDepthNm = 0`.
  - `PSFParam_PsfInterp = Fft` (Fourier-shift placement: slow, CPU-only;
    `FftShiftKernelTile` does webSMLM's 2D shift as separable row/column
    1D shifts -- the same linear operation, ~170 s per default 1000-frame
    stack instead of tens of minutes).
  - A Cramer-Rao bound summary is logged to the corelog after every kernel
    compute (`DescribePsfCramerRao`).
  - The double-helix PSF is wide (lobes ~0.8 um off axis at focus, only
    ~11% of the light in them -- same as webSMLM); a small kernel
    half-width truncates it.
- **Photophysics**: `EmitterModel` has a "rich" path used only when
  `BlinkBleachProb < 1`, `PhotonCV > 0`, or for the out-of-focus
  population. That path is molecules with geometric blinks, exponential
  dark time and log-normal per-blink brightness, with the arrival rate
  divided by the mean blink count so density keeps meaning ON-density.
  Otherwise the original single-blink draw sequence is untouched. Live mode
  carries `BlinkEvent::moleculeLive` and schedules each surviving
  molecule's next blink when its ON period ends.
- **Illumination / background** (`Simulation/SMLMBackground.h/.cpp`):
  - The peak-normalized illumination field multiplies background and
    emitters (read at the undrifted site).
  - The cell + haze background map is normalized to the FOV mean.
  - The fade is `0.3 + 0.7 exp(-t/decay)`.
  - The haze uses 20000 `SampleSite` draws (continuous patterns have no
    site list).
  - Out-of-focus emitters run at `|z|` in [300 nm, depth], clamped to the
    kernel's z range, and need a vectorial model. webSMLM's lower bound is
    max(astigmatic usable range, 300 nm); this project doesn't compute that
    range, so it always uses 300.
- **Camera**: EMCCD path in `ApplyNoiseChain`, which keeps demoCam's dark
  current (webSMLM has none). The sCMOS path keeps QE, dark current and the
  per-pixel maps (demoCam is ahead there).
- **Drift**: random direction per seed (`DriftAngleForSeed`, its own
  stream) instead of the fixed 2:1 diagonal.
- **Pattern**: `FilamentsRing`, webSMLM's default structure, with its
  y-correlated z; the px constants are converted at 100 nm/px.
- **Speed** (webSMLM 21b/21c analog):
  - `PsfKernelCache` planes are sum-1 normalized with precomputed
    oversampling x oversampling block sums (`BuildBlockSums`), so the splat
    does one interpolation per camera pixel (`SplatSetup`).
  - Frames render only their own events (`BucketEventsByFrame`).
  - Camera noise uses counter-based pcg4d draws (`SMLMCounterRng.h`), so
    frames are independent.
  - The precomputed stack renders on all CPU cores or on the GPU
    (`Simulation/GpuSimD3D11.*`: one fused D3D11 compute shader, HLSL
    compiled at runtime by D3DCompile, frames batched per dispatch,
    WARP/software adapters rejected).
  - Measured, 128x128 x 1000 frames, default GibsonLanniZernike, 12-thread
    laptop with Iris Xe: render ~0.5 s CPU / 0.15 s GPU, vs ~170-190 s
    before.
  - GPU and CPU agree on >= 99.8% of pixels; the rest are float32 rounding,
    a Poisson draw one electron apart.
  - `General_UseGpu` (default On) / `General_GpuStatus` (read-only: adapter
    or reason for CPU).

Default-output changes in this round (all deliberate):
- The counter-based noise RNG changes every seeded frame once, as in
  webSMLM 21b.
- The splat now normalizes whole kernel planes (not each emitter's window)
  and rounds Nearest placement to the nearest grid point (the old code
  floored it, a 1/(2*oversampling) px bias).
- Preset values follow webSMLM.
- Drift direction is random per seed.

Gotcha found while verifying this: the old sCMOS noise was **never byte-
reproducible across rebuilds**. `CombinedShotAndReadNoise` drew
`PoissonRng(rng) + rn * GaussianRng(rng)` in one expression, whose operand
evaluation order C++ leaves unspecified, so an unrelated recompile could
swap the two draws. The counter-based chain draws in explicit statement
order. Never put two draws from the same sequential rng in one expression.

Not ported (by design):
- psfmle fitting and GT scoring/export -- analysis-side;
- the realism/density presets -- they only write other parameters;
- webSMLM's `zUsableNm` metric.

**Parity-doc staleness reminder:** `PARITY.md` (in `C:\GitHub\websmlm`) is
a point-in-time snapshot, not auto-updated, and this repo has no visibility
into that project's future commits (it has other collaborators). Before
starting *any new* simulation-engine work here, re-check `PARITY.md`
against websmlm's current `webSMLM.html` and refresh it if it's drifted.
