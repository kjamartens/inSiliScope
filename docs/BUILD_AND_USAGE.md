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
4. On the pre-init properties page, optionally set `RandomSeed` -- this is
   the only pre-init property. `FovSize` (`128x128`/`256x256`/`512x512`,
   default `512x512`) is a regular property, changeable at any time after
   initialization too.
5. Finish the wizard and initialize.

## Using it

There is deliberately only one exposure/timing control: the standard MM
`Exposure` property. `EmitterDensityPerSec`, `OnLifetimeSec`,
`PhotonsPerSecond`, and `BackgroundPhotonsPerSec` are all expressed as rates
(per second) and automatically scale to whatever `Exposure` is currently set
to -- e.g. doubling `Exposure` doubles the photons and background collected
per frame, exactly as changing a real camera's exposure time would.

- **Live mode** (default): a background thread continuously simulates
  frames at the current `Exposure` interval, starting as soon as the device
  initializes. Every property that affects simulated frame content can be
  changed while streaming and is picked up automatically on one of the next
  few ticks -- `Pattern`, `CustomPointsFile`, `ResolutionSpacingsNm`,
  `FovSize`, `Binning`, `EmitterDensityPerSec`, `PhotonsPerSecond`, `OnLifetimeSec`,
  `PsfEmissionWavelengthNm`, `PsfNa`, `PixelSizeNm`, `BackgroundPhotonsPerSec`,
  `CameraGainPhotonsPerADU`, `CameraOffsetADU`, `CameraOffsetStdADU`,
  `ReadNoiseElectrons`, `DriftNmPerSec`. The read-only `ActualFrameIntervalMs`
  property reports a rolling average (last 10 frames) of the actual
  wall-clock time between frames the producer thread publishes -- useful for
  spotting when it's running behind the requested `Exposure` (e.g. very
  dense/large frames), as opposed to the requested interval itself.
  Changing `EmitterDensityPerSec`/`OnLifetimeSec` affects *new* blinks only;
  emitters already mid-blink finish out their originally-drawn ON interval,
  same as changing `Pattern` doesn't retroactively move already-spawned
  emitters. This is driven by a single internal "config changed" signal that
  every relevant property handler raises, so newly added parameters are
  covered automatically rather than needing individual wiring.
- **Precomputed mode**: set `AcqMode=Precomputed`. Set `Pattern`,
  `StackLength`, and the simulation parameters above, then write `1` to
  `GenerateStack` (generation also auto-triggers on first Snap/Live/sequence
  use if you never touch it, and re-triggers automatically whenever any
  simulation parameter, `Pattern`, `Binning`, `FovSize`, `Exposure`, or
  `RandomSeed` changes). Poll `StackGenerationStatus` -- or just Snap; it
  blocks until ready -- until it reads `Ready (N frames)`. Then Snap, Live,
  or run a Multi-D Acquisition as usual; the movie will loop
  (`StackLoop=On`, default) or clamp at the last frame (`StackLoop=Off`, in
  which case `EndOfStackReached` becomes `Yes`). Stopping a Live/sequence
  acquisition never blocks on generation finishing -- it returns promptly
  even mid-generation.

### Drift

`DriftNmPerSec` is a stage-drift rate, in nm/sec, applied along X; Y drifts
at half that rate (a fixed diagonal direction, not random). It works the
same way in both acquisition modes and always starts at zero: every new
Live or Multi-D acquisition (i.e. each `StartSequenceAcquisition`, whether
from the Live button or an MDA run) resets the drift ramp to its origin, so
drift never carries over mid-ramp from a previous run. In Precomputed mode
this also means the ramp restarts from frame 0 of the stack even if a
previous acquisition had advanced partway through it (or looped several
times via `StackLoop`).

### PSF size

Instead of a raw pixel sigma, the PSF is specified physically via
`PsfEmissionWavelengthNm` (dye emission wavelength, nm; default 660, a
typical red dye like Alexa647/Cy5) and `PsfNa` (objective numerical
aperture; default 1.4, a typical oil-immersion objective). The rendered
Gaussian sigma is the standard diffraction-limited approximation
`sigma ~= 0.21 * wavelength / NA`, converted to pixels via `PixelSizeNm`.

### Patterns

Built-in patterns: `Circle`, `Lines`, `Grid`, `Random`, `Spiral`, `Star`,
`Heart`, `ResolutionTarget`, and `CustomPoints` (loaded from a CSV file of
normalized `x,y` coordinates via `CustomPointsFile`).

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
`ResolutionSpacingsNm` property: a comma-separated list of nm values, editable
like any other property (including while streaming in Live mode). Any
positive number of values is accepted; `ResolutionTarget`'s grid always lays
itself out as close to square as the count allows, so it isn't limited to a
3x3/9-value list.

## Automated smoke test

```
pip install pymmcore-plus numpy
python tools/test_smlmcam.py
```

Exercises pre-init property enforcement, background stack generation,
reproducibility (same `RandomSeed` + params -> byte-identical stack), and
both acquisition modes end-to-end.
