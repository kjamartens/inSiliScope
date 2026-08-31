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
Zernike patch regardless. **Step 5 (Zernike aberrations) is not started.**

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
   for why this matters):
   ```
   cd DeviceAdapter/SMLMDemoCam/Simulation/psfbridge-java
   javac --release 8 -cp ../../../../third_party/PSFGenerator.jar -d out psfbridge/PsfBridge.java
   ```
4. Merge the compiled class into a copy of PSFGenerator.jar:
   ```
   cp third_party/PSFGenerator.jar third_party/SMLMPsfEmbedded.jar
   cd DeviceAdapter/SMLMDemoCam/Simulation/psfbridge-java/out
   jar uf ../../../../../third_party/SMLMPsfEmbedded.jar psfbridge/PsfBridge.class
   ```
5. Build the solution (MSBuild picks up `third_party/SMLMPsfEmbedded.jar`
   via `SMLMDemoCam.rc` and embeds it into the DLL automatically):
   ```
   MSBuild.exe SMLMDemoCam.sln /p:Configuration=Release /p:Platform=x64
   ```

Whenever `psfbridge/PsfBridge.java` changes, repeat steps 3-5 (the merged
jar must be rebuilt and the DLL relinked -- MSBuild's resource compiler
only re-embeds when the `.jar` file's timestamp changes).

A JRE/JDK must be present at runtime too (to supply `jvm.dll`) -- see
`PsfGeneratorJavaHome` property below.

### New MM properties

- `PsfModel` -- `Gaussian` | `RichardsWolf` | `GibsonLanni` (default:
  `GibsonLanni`)
- `PsfImmersionIndex`, `PsfOversampling` (default 12), `PsfKernelHalfWidthPx`
  (a *minimum*; auto-grown from NA/wavelength/pixel size -- see Gotchas)
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

### Next steps

See `docs/vectorial-psf-plan.md` for the full write-up. In order:

1. ~~Step 1: in-focus 2D vectorial PSF~~ -- done
2. ~~Step 2: real Z-stack (`nz > 1`) + random per-emitter Z spread~~ --
   code-complete, not yet visually verified
3. ~~Step 3: revert random Z spread; add a real `MM::Stage` device
   (`SMLMDemoZStage`) for a global, user-drivable focus offset~~ --
   code-complete, not yet visually verified
4. Step 4: compare this plugin's PSF + noise models against the Sage et
   al. 2019 "Super-Resolution Fight Club" SMLM Challenge methodology
   (research-only, informs step 5's Zernike defaults)
5. Step 5: static Zernike aberrations -- requires patching a local
   PSFGenerator fork (`RichardsWolfPSF` is radially-symmetric only, no
   azimuthal/Zernike support in the stock library)
