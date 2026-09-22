# Build and usage

## Prerequisites

- Windows, Visual Studio 2022 with the "Desktop development with C++"
  workload (MSVC v143 toolset, Windows SDK).
- Python with [pymmcore-plus](https://pymmcore-plus.readthedocs.io/) if you
  want to install a matching Micro-Manager nightly and run the smoke test:
  `pip install pymmcore-plus`.

## Getting the source

```
git clone <this repo>
cd demoCam_SMLM_MM
git submodule update --init --recursive
```

The `third_party/mmCoreAndDevices` submodule provides the MMDevice SDK
headers and the shared MSBuild `.props` files every Micro-Manager device
adapter builds against.

## Installing a matching Micro-Manager nightly

The device-interface version compiled into `mmgr_dal_SMLMDemoCam.dll` must
match the Micro-Manager build you load it into. Stable MM releases are often
too old; use a nightly instead:

```
mmcore install
mmcore list   # shows the active install path
```

Set the `MM_DIR` environment variable if you want to point at a specific
install instead of the pymmcore-plus-managed one.

## Building

Open `DeviceAdapter/SMLMDemoCam/SMLMDemoCam.sln` in Visual Studio 2022,
select `Release|x64`, and build. The solution also builds
`MMDevice-SharedRuntime` (from the submodule) as a dependency. Output lands
at `DeviceAdapter/SMLMDemoCam/build/Release/x64/mmgr_dal_SMLMDemoCam.dll`.

## Installing into Micro-Manager

Copy `mmgr_dal_SMLMDemoCam.dll` into your Micro-Manager install directory
(the same folder as `MicroManager.exe` / `ImageJ.exe` -- e.g. the directory
`mmcore list` reports as active). No manifest file is needed; Micro-Manager
discovers device adapters by scanning its install directory for
`mmgr_dal_*.dll`.

## Adding the device in Micro-Manager

1. Launch Micro-Manager Studio.
2. Tools -> Hardware Configuration Wizard -> add a new device.
3. Find `SMLMDemoCam` in the device list and add it as a camera.
4. On the pre-init properties page, optionally set `SimType_RandomSeed` -- this is
   the only pre-init property. `General_FovSize` (`128x128`/`256x256`/`512x512`,
   default `512x512`) is a regular property, changeable at any time after
   initialization too.
5. Finish the wizard and initialize.

## Using it

There is deliberately only one exposure/timing control: the standard MM
`Exposure` property. `General_EmitterDensityPerSec`, `FluoParam_OnLifetimeSec`,
`FluoParam_PhotonsPerSecond`, and `Background_BackgroundPhotonsPerSec` are all expressed as rates
(per second) and automatically scale to whatever `Exposure` is currently set
to -- e.g. doubling `Exposure` doubles the photons and background collected
per frame, exactly as changing a real camera's exposure time would.

- **Live mode** (default): a background thread continuously simulates
  frames at the current `Exposure` interval, starting as soon as the device
  initializes. Every property that affects simulated frame content can be
  changed while streaming and is picked up automatically on one of the next
  few ticks -- `SimType_Pattern`, `SimType_CustomPointsFile`, `SimType_ResolutionSpacingsNm`,
  `General_FovSize`, `Binning`, `General_EmitterDensityPerSec`, `FluoParam_PhotonsPerSecond`, `FluoParam_OnLifetimeSec`,
  `PSFParam_PsfEmissionWavelengthNm`, `PSFParam_PsfNa`, `General_PixelSizeNm`, `Background_BackgroundPhotonsPerSec`,
  `CamParam_GainPhotonsPerADU`, `CamParam_OffsetADU`, `CamParam_OffsetStdADU`,
  `CamParam_ReadNoiseElectrons`, `SimType_DriftNmPerSec`. The read-only `General_ActualFrameIntervalMs`
  property reports a rolling average (last 10 frames) of the actual
  wall-clock time between frames the producer thread publishes -- useful for
  spotting when it's running behind the requested `Exposure` (e.g. very
  dense/large frames), as opposed to the requested interval itself.
  Changing `General_EmitterDensityPerSec`/`FluoParam_OnLifetimeSec` affects *new* blinks only;
  emitters already mid-blink finish out their originally-drawn ON interval,
  same as changing `SimType_Pattern` doesn't retroactively move already-spawned
  emitters. This is driven by a single internal "config changed" signal that
  every relevant property handler raises, so newly added parameters are
  covered automatically rather than needing individual wiring.
- **Precomputed mode**: set `AcqMode=Precomputed`. Set `SimType_Pattern` and
  the simulation parameters above, then write `1` to
  `General_GenerateStack` (generation also auto-triggers on first Snap/Live/sequence
  use if you never touch it, and re-triggers automatically whenever any
  simulation parameter, `SimType_Pattern`, `Binning`, `General_FovSize`, `Exposure`, or
  `SimType_RandomSeed` changes). Poll `General_StackGenerationStatus` -- or just Snap; it
  blocks until ready -- until it reads `Ready (N frames)`. Then Snap, Live,
  or run a Multi-D Acquisition as usual; the movie loops back to frame 0 at
  the end. The stack is a fixed 1000 frames and always loops -- both used to
  be the `General_StackLength`/`General_StackLoop` properties, since removed;
  `General_EndOfStackReached` is consequently always `No`. A generation
  pass at the default `PSFParam_PsfModel = GibsonLanniZernike` takes a few
  seconds, mostly the one-time PSF kernel computation; the 1000 frames
  themselves render in well under a second on the GPU or a multi-core CPU
  (see "GPU" below). `PSFParam_PsfInterp = Fft` is the exception: one 2D
  FFT pair per emitter makes it minutes. Stopping a Live/sequence acquisition never blocks on generation
  finishing -- it returns promptly even mid-generation.

### Drift

`SimType_DriftNmPerSec` is a stage-drift speed, in nm/sec, along a direction
drawn once per `SimType_RandomSeed` (random like webSMLM's, but reproducible
for a given seed). It works the
same way in both acquisition modes and always starts at zero: every new
Live or Multi-D acquisition (i.e. each `StartSequenceAcquisition`, whether
from the Live button or an MDA run) resets the drift ramp to its origin, so
drift never carries over mid-ramp from a previous run. In Precomputed mode
this also means the ramp restarts from frame 0 of the stack even if a
previous acquisition had advanced partway through it (or looped several
times).

### PSF size

Instead of a raw pixel sigma, the PSF is specified physically via
`PSFParam_PsfEmissionWavelengthNm` (dye emission wavelength, nm; default 660, a
typical red dye like Alexa647/Cy5) and `PSFParam_PsfNa` (objective numerical
aperture; default 1.4, a typical oil-immersion objective). The rendered
Gaussian sigma is the standard diffraction-limited approximation
`sigma ~= 0.21 * wavelength / NA`, converted to pixels via `General_PixelSizeNm`.

### Patterns

Built-in patterns: `Circle`, `Lines`, `Grid`, `Random`, `Spiral`, `Star`,
`Heart`, `ResolutionTarget`, and `CustomPoints` (loaded from a CSV file of
normalized `x,y` coordinates via `SimType_CustomPointsFile`).

`Circle`, `Spiral`, `Star`, and `Heart` are each their own miniature
resolution test: instead of one filled/stroked outline, each renders as
several concentric rings/scaled copies (or, for `Spiral`, several arcs along
one continuous spiral), where every step is actually **two parallel lines**
whose gap shrinks from step to step. Scan from the innermost/first step
outward to see the point at which the current PSF size / pixel size /
emitter density can no longer resolve the two lines as separate.

`ResolutionTarget` is the same idea laid out as a classic resolution chart
(à la USAF 1951): a roughly-square grid of cells, each showing a small group
of parallel lines at one spacing, assigned row-major.

The step/spacing sequence all five of these patterns draw from -- default
500, 300, 200, 100, 80, 50, 30, 20, 10 nm, easiest to hardest -- is the
`SimType_ResolutionSpacingsNm` property: a space-separated list of nm values
(commas and semicolons are accepted too when set from code, but MMCore rejects
a comma in any property value set through it), editable
like any other property (including while streaming in Live mode). Any
positive number of values is accepted; `ResolutionTarget`'s grid always lays
itself out as close to square as the count allows, so it isn't limited to a
3x3/9-value list.

### Blinking, brightness and illumination

- `FluoParam_BlinkBleachProb` (default 1): chance an ON period ends in
  bleaching. Below 1 each molecule blinks a geometric number of times (mean
  1/p), dark for `FluoParam_OffLifetimeSec` (mean, exponential) between
  blinks, at a persistent position. `General_EmitterDensityPerSec` keeps
  meaning the density of ON emitters.
- `FluoParam_PhotonCV` (default 0): per-blink log-normal photon-rate spread
  (mean preserved).
- `FluoParam_IllumProfile` (`Flat` | `Gaussian` | `FlatTop`) with
  `FluoParam_IllumFwhmPct` (% of FOV width): excitation profile, peak 1, so
  the photon and background settings are the values at the beam centre.

### Camera: sCMOS or EMCCD

`CamParam_CameraType = EMCCD` replaces shot + read noise with an
electron-multiplying register: Poisson(QE x photons + dark + CIC) electrons
-> Gamma(electrons, 1) (variance 2x the mean) -> read noise divided by
`CamParam_EmGain` -> `/CamParam_GainPhotonsPerADU` -> offset, rounded and
clipped to `CamParam_BitDepth` bits. `CamParam_CicElectrons` is the
clock-induced charge per pixel per frame.

### Structured background

All `Background_` properties; each defaults to off.

- `Background_BackgroundPhotonsPerSec`: mean background, photons/pixel/s.
- `Background_CellContrast` (> 1): a soft-edged cell outline that many
  times brighter than its surroundings, FOV mean unchanged.
- `Background_HazeWeight` / `Background_HazeWidthNm`: static out-of-focus
  haze, the structure's own projected density blurred by that width.
- `Background_DecaySec`: the background fades to a 30% floor with this
  time constant (from frame 0 / the start of each Live acquisition).
- `Background_OutOfFocusRatio` / `Background_OutOfFocusDepthNm`: a second,
  blinking population on the same structure, 300 nm to the given depth
  above/below focus, rendered through the real defocused PSF. It needs a
  vectorial `PSFParam_PsfModel` and a z stack (`PSFParam_PsfZRangeUm`).

### Engineered PSFs

`PSFParam_PsfZernikePreset` includes `SaddlePoint`, `ExtendedRange` and
`ExtendedRangeStrong`, stacked astigmatism (OSA j = 5/13/25) for a longer
single-valued z range. `PSFParam_PsfZernikeCoefficients` holds 28
space-separated values in waves (OSA 0-27); a 15-value list is still
accepted and zero-padded. `PSFParam_PsfMaskType = DoubleHelix` adds a
Gauss-Laguerre double-helix phase mask (`PSFParam_PsfMaskModes`,
`PSFParam_PsfMaskWaist`) with two lobes that rotate with defocus. That PSF
is wide, so keep `PSFParam_PsfKernelHalfWidthNm` at its 3000 nm default or
larger. All of these apply to `GibsonLanniZernike` only. After every kernel
computation the corelog reports the PSF's x/y/z Cramer-Rao bound at the
current photon count and background.

### GPU

`General_UseGpu` (default `On`) renders vectorial-PSF frames, splat plus
camera noise, as one Direct3D 11 compute shader. Direct3D 11 ships with
Windows and works on any vendor's GPU, integrated ones included. No extra
install is needed. `General_GpuStatus` names the adapter in use, or says why
the CPU path is in use instead:
- the Gaussian model;
- `PsfInterp = Fft`;
- no hardware GPU;
- `UseGpu = Off`.

The CPU path uses every core. Both paths draw their noise from the same
counter-based random stream, so a seeded stack is the same either way,
up to float32 rounding: about 1 pixel in 10^4-10^5 lands one photo-electron
apart.

## Automated smoke test

```
pip install pymmcore-plus numpy
python tools/test_smlmcam.py
```

Exercises pre-init property enforcement, background stack generation,
reproducibility (same `SimType_RandomSeed` + params -> byte-identical stack),
both acquisition modes end-to-end, and the webSMLM-parity features:
- multi-blink density;
- the EMCCD variance/mean ratio;
- background shape and fade;
- out-of-focus emitters;
- the double-helix mask;
- GPU vs CPU agreement (skipped without a GPU).
