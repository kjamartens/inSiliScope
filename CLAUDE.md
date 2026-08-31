# demoCam_SMLM_MM

A synthetic SMLM (Single-Molecule Localization Microscopy) camera device
adapter for Micro-Manager. Generates blinking-fluorophore movies with
realistic PSFs and camera noise, either as a reproducible precomputed stack
or a live-streaming mode with parameters adjustable while running.

- Device adapter source: `DeviceAdapter/SMLMDemoCam/`
- Core simulation engine (no MMDevice dependency): `DeviceAdapter/SMLMDemoCam/Simulation/`
- Build: `DeviceAdapter/SMLMDemoCam/SMLMDemoCam.sln` (MSBuild, Release|x64),
  outputs `mmgr_dal_SMLMDemoCam.dll` to `DeviceAdapter/SMLMDemoCam/build/Release/x64/`

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
`PsfGeneratorJavaHome` property below.

### New MM properties

- `PsfModel` -- `Gaussian` | `RichardsWolf` | `GibsonLanni` |
  `GibsonLanniZernike` (default: `GibsonLanni`)
- `PsfImmersionIndex`, `PsfOversampling` (default 4, was 12 before step 5 --
  lowered so the out-of-the-box oversampled kernel stays small regardless
  of `PsfModel`, see the step 5 performance Gotcha), `PsfKernelHalfWidthPx`
  (default 16, was 32 -- same reason; a *minimum*, auto-grown from NA/
  wavelength/pixel size -- see Gotchas)
- `PsfGeneratorJavaHome` -- optional JRE/JDK root override; auto-detects
  otherwise (`JAVA_HOME`, then common Windows install paths)
- `PsfZRangeUm` (default 2.0), `PsfZStepUm` (default 0.1) -- Z-stack range/
  step (step 2). The per-emitter random Z spread step 2 originally added
  here (`PsfZSpreadStdNm`) was removed again in step 3, replaced by a real
  `SMLMDemoZStage` device (`MM::Stage`) driving one global, user-drivable
  focus offset -- see `Simulation/SharedStageState.h`. Add both
  "SMLMDemoCam" and "SMLMDemoZStage" via the Hardware Configuration Wizard;
  they communicate through a process-wide singleton, no explicit MM
  device-linking needed.
- `PsfSampleIndex`, `PsfWorkingDistanceUm` (default 150.0),
  `PsfSampleDepthNm` -- GibsonLanni-only PSFGenerator parameters, ignored
  by RichardsWolf; `PsfSampleIndex` defaults to matching
  `PsfImmersionIndex` and `PsfSampleDepthNm` defaults to 0, reproducing
  PsfBridge.java's old hardcoded no-mismatch/in-focus behavior rather than
  PSFGenerator's own stock defaults (1.33 / 2000)
- `PsfZernikeCoefficients` (string, 15 comma-separated values, default all-
  zero) -- `GibsonLanniZernike`-only (step 5); positional OSA/ANSI single
  Zernike index 0-14, in waves. See `Simulation/SMLMZernike.h`'s
  `ZernikeCoefficients` doc comment for the exact index-to-mode mapping (0
  piston ... 5 vertical astigmatism ... 7/8 coma ... 12 primary spherical
  ... 14 vertical quadrafoil).
- `PsfZernikePreset` -- convenience dropdown on top of
  `PsfZernikeCoefficients`: `None` (default) | `AstigmatismWeak` |
  `AstigmatismModerate` | `AstigmatismStrong` | `ComaWeak` | `ComaStrong` |
  `SphericalWeak` | `SphericalStrong` | `TrefoilModerate` |
  `MixedRealisticObjective`. Selecting one overwrites
  `PsfZernikeCoefficients` with a named, order-of-magnitude wavefront-error
  estimate (~0.07-0.15 waves = mild, ~0.2-0.3 waves = strong -- the classic
  Marechal/diffraction-limit criterion is ~0.07 waves RMS, lambda/14) --
  **not numbers sourced from a specific paper**, same "gap to flag
  explicitly" stance as `docs/vectorial-psf-step4-smlm-challenge-
  comparison.md` already takes for this project's one pre-existing
  illustrative Zernike value. See `Simulation/SMLMZernike.cpp`'s
  `ZernikePresetCoefficients` for the exact values. Editing
  `PsfZernikeCoefficients` directly afterwards does not update/clear this
  property -- it only ever reports the last preset explicitly selected
  through it.

### Noise model properties (Kinetix22 sCMOS defaults)

`Simulation/SMLMNoise.h/.cpp`'s chain: QE -> + dark current -> Poisson shot
noise -> (per-pixel or scalar) Gaussian read noise -> (per-pixel or scalar)
gain -> static per-pixel additive offset -> 16-bit clamp. New properties
`QuantumEfficiency`, `DarkCurrentElectronsPerSec`, `PixelGainStdPct`,
`PixelReadNoiseStdPct` (the last two are relative pixel-to-pixel spread on
top of `CameraGainPhotonsPerADU`/`ReadNoiseElectrons`, sCMOS-style;
`0` = every pixel identical, matching the `DriftNmPerSec = 0`
disabled-by-default convention). Defaults for `QuantumEfficiency`
(0.85), `DarkCurrentElectronsPerSec` (1.03), `CameraGainPhotonsPerADU`
(0.25), and `ReadNoiseElectrons` (1.2) match the Photometrics Kinetix22
sCMOS Sensitivity (CMS) mode datasheet; `PixelGainStdPct`/
`PixelReadNoiseStdPct` default to 5%/20% as estimates, since Photometrics
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
  normal photon counts. Cranking `PhotonsPerSecond` up does *not* help
  reveal it (makes the peak-to-ring dynamic range worse on a linear
  display); a log/gamma LUT does.
- **`PsfKernelHalfWidthPx` is a minimum, not the actual value used** --
  `BuildPsfGeneratorRequest()` auto-grows it to 3x the Rayleigh radius
  (`0.61*lambda/NA`) so the rendered window can't truncate the first ring
  regardless of NA/wavelength/pixel size, capped at 48px.
- **`CStageBase<U>` does not default-implement `IsStageSequenceable`**
  (only `IsStageLinearSequenceable`) -- omitting an override leaves the
  device class abstract and fails to compile/instantiate with `C2259`
  ("cannot instantiate abstract class") at the `new SMLMDemoZStage()` call
  site in the module's `CreateDevice`, not in `SMLMDemoZStage.h/.cpp`
  itself, which can make the error confusing to trace back.

### Gotchas found during step 5 (don't rediscover these)

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
  this is enormous**: `PsfZRangeUm`/`PsfZStepUm` defaults alone give 70+ Z
  planes, and `PsfOversampling`/`PsfKernelHalfWidthPx` can put the
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
    up-front warning (with concrete advice: lower `PsfOversampling`/
    `PsfKernelHalfWidthPx`/`PsfZRangeUm`/`PsfZStepUm`) whenever
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
  - `PsfOversampling`/`PsfKernelHalfWidthPx` defaults were lowered to 4/16
    (from 12/32) specifically because of this model's cost profile -- see
    the property list above. This is the biggest lever of all: kernel
    pixel count is `O((kernelHalfWidthPx*oversampling)^2)`, so 12/32 -> 4/16
    alone is roughly a 28x reduction in oversampled-kernel pixel count
    before any algorithmic change.
  - Benchmarked (this dev machine, 12 logical cores, post all of the above):
    65x65px x 24 planes ~0.5-0.6s; 129x129px x 24 planes ~2.4-3.1s. Both
    scale roughly with `size^2 * nz`, so a user-chosen 769x769 x 71
    combination (achievable by raising `PsfOversampling`/
    `PsfKernelHalfWidthPx`/`PsfZRangeUm` well past their new defaults)
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
   `PsfModel = GibsonLanniZernike`, try a `PsfZernikePreset`, confirm it
   looks like existing `GibsonLanni` at all-zero and visibly aberrates
   otherwise) is the one remaining piece of this feature.
