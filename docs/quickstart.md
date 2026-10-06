# Quickstart

## In the browser

Open the [viewer](try-viewer.md). Nothing to install.

## Micro-Manager

1. Download `mmgr_dal_inSiliScope.dll` and the configurations `inSiliScope_Basic.cfg`, `inSiliScope_Advanced.cfg` and
   `inSiliScope_Expert.cfg` from the latest [release](https://github.com/kjamartens/inSiliScope/releases) (verify the
   DLL with its `.sha256` file) and copy them into your Micro-Manager folder.
   The default PSF (`GibsonLanniZernike`) and the Gaussian need nothing else. A Java runtime is needed only for the
   `RichardsWolf` and `GibsonLanni` models (the DLL embeds PSFGenerator and starts, or attaches to, a JVM).
2. Start Micro-Manager with `inSiliScope_Basic.cfg`. It loads the hub `inSiliScope` and one device per part of the
   microscope: `Camera`, `XYStage`, `ZStage`, `Objective`, `EmissionPath`, `FilterCube`, `Lasers`, `TransmittedLamp`,
   `SampleHolder`, `CellField`, `Fluorophores` and `Renderer`, with the config groups
    - `Channel`: one preset per label mode with its typical dye (`AF647 dSTORM`, `mEos3.2 PALM`, `ATTO655 DNA-PAINT`,
      `mEGFP WideField`) and `BrightField`;
    - `Objective`, `Camera` (Kinetix22 with a 0.667x relay: 97.45 nm pixels at 100x; iXon Ultra 897 with 1.6x: 100 nm),
      `Quality` (Fast / Realistic / Exhaustive) and `Specimen`;
    - and the pixel-size calibration of every objective and camera.

   `inSiliScope_Advanced.cfg` adds the excitation filter, dichroic and emission filter wheels, a `Drift` group and the
   channel `mEGFP WideField + BrightField`; `inSiliScope_Expert.cfg` shows every property. The hub's pre-init `Detail`
   (Basic, Advanced, Expert) decides which properties a session shows; to build a configuration of your own, add the
   hub `inSiliScope` in the Hardware Configuration Wizard, then its devices.
3. Take a snapshot of the cell field, move the XY stage, change the Z stage. The Z stage position is the focal plane
   height above the coverslip (0 = coverslip in focus; it starts at 0.5 um).
4. Pick a `Channel`: the label mode and its typical dye; the lasers and the filter cube follow (`Lasers.Preset`).
   `CellField.Microtubules_Label` offers the other dyes with data for the mode (`Fluorophores.Mode`); custom dyes are
   `Fluorophores.Dye1..3_*` (Expert).
5. The light comes from the shutters: `Lasers` (fluorescence) and `TransmittedLamp` (BrightField). With both open
   (the Utilities `Multi Shutter`, as in the Advanced channel `mEGFP WideField + BrightField`) the camera sums the two;
   with none open it takes dark frames. The autoshutter opens the Core-Shutter for each image.

Every device and property, with its tier and default: [Micro-Manager properties](mm-properties.md). Snaps, live mode and
sequences (also hardware z stacks: the `ZStage` is sequenceable) render live. `Renderer.WriteScopeSpecTo` (Expert)
writes the settings of the next frame to a file that `insiliscope_cli --spec <file>` renders. The camera remembers how
long each place of the sample has been lit (snaps, live acquisition and sequences add to it): imaging bleaches and uses
up dyes where you imaged, a place never lit starts fresh (dSTORM dyes first in their bright initial ON phase).

The adapter computes the PSF kernel in the background as soon as the device initialises, so the first frame does not
wait for it. `Renderer.DiskCache` (default `Cells`) keeps the packed cell positions of the field in a small per-user file
(`%LOCALAPPDATA%\inSiliScope\cache`, or `$ISC_CACHE_DIR`), so a restart with the same seed and cell parameters starts
with the cells in place; `CellsAndPsf` also stores the PSF kernel (one file of ~200 MB at the defaults, read in about
half the time it takes to compute); `Off` writes nothing.

From Python (pymmcore-plus):

```python
from pymmcore_plus import CMMCorePlus
core = CMMCorePlus()
core.setDeviceAdapterSearchPaths([r"C:\Program Files\Micro-Manager-2.0"])
core.loadSystemConfiguration(r"C:\Program Files\Micro-Manager-2.0\inSiliScope_Basic.cfg")
core.setConfig("Channel", "mEGFP WideField")
core.snapImage(); img = core.getImage()
```

## Command line

```
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --target insiliscope_cli
build/native/cli/insiliscope_cli --out movie.tif --frames 200 --size 128 --seed 42
build/native/cli/insiliscope_cli --help          # every option and its default
build/native/cli/insiliscope_cli --prepare 1 --disk-cache 2   # warm the caches (world, cells, PSF kernel), no movie
```

With `--drift-xy-speed-nm-per-sec` / `--drift-z-speed-nm-per-sec` the sample drifts (mean speeds; see `--help` for
the direction, its wander and the random-walk jitter `--drift-xy-nm-per-sqrt-sec`) and the CLI writes the true drift per
frame to `movie.drift.csv`.

The CLI keeps the packed cell positions in the same per-user cache directory as the adapter (`--disk-cache 1`, the
default); `--disk-cache 2` adds the PSF kernel, `0` writes nothing.

## webSMLM

Each release carries `cellfield_block.js`. In the webSMLM repository run
`node tools/sync_cellfield.mjs <path to cellfield_block.js>`.

## Build from source

See `CLAUDE.md` in the repository for the full build and test matrix (core native and WASM, adapter MSBuild,
golden vectors, GPU checks).
