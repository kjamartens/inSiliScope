# Quickstart

## In the browser

Open the [viewer](try-viewer.md). Nothing to install.

## Micro-Manager

1. Download `mmgr_dal_inSiliScope.dll` from the latest [release](https://github.com/kjamartens/inSiliScope/releases)
   (verify with the `.sha256` file) and copy it into your Micro-Manager folder.
   The default PSF (`GibsonLanniZernike`) and the Gaussian need nothing else. A Java runtime is needed only for the
   `RichardsWolf` and `GibsonLanni` models (the DLL embeds PSFGenerator and starts, or attaches to, a JVM).
2. In the Hardware Configuration Wizard add the module **inSiliScope** and the devices `Camera`, `XYStage` and `ZStage`.
   The devices share state in-process; no linking is needed.
3. Choose the `CellField` pattern (the default), take a snapshot, move the XY stage, change the Z stage. The Z stage
   position is the focal plane height above the coverslip (0 = coverslip in focus; it starts at 0.5 um).
4. Switch `General_ImagingModality` between `SuperRes`, `WideField` and `BrightField` (transmitted light;
   `General_BrightFieldQuality` 1-4 trades speed for precision).

Property names are grouped by prefix: `General_`, `SimType_`, `FluoParam_`, `CamParam_`, `PSFParam_`, `Background_`.

The adapter computes the PSF kernel in the background as soon as the device initialises, so the first frame does not
wait for it. `General_DiskCache` (default `Cells`) keeps the packed cell positions of the field in a small per-user file
(`%LOCALAPPDATA%\inSiliScope\cache`, or `$ISC_CACHE_DIR`), so a restart with the same seed and cell parameters starts
with the cells in place; `CellsAndPsf` also stores the PSF kernel (one file of ~200 MB at the defaults, read in about
half the time it takes to compute); `Off` writes nothing.

From Python (pymmcore-plus):

```python
from pymmcore_plus import CMMCorePlus
core = CMMCorePlus()
core.setDeviceAdapterSearchPaths([r"C:\Program Files\Micro-Manager-2.0"])
core.loadDevice("Camera", "inSiliScope", "Camera")
core.loadDevice("XYStage", "inSiliScope", "XYStage")
core.loadDevice("Z", "inSiliScope", "ZStage")
core.initializeAllDevices()
core.setCameraDevice("Camera")
core.setProperty("Camera", "General_ImagingModality", "WideField")
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

The CLI keeps the packed cell positions in the same per-user cache directory as the adapter (`--disk-cache 1`, the
default); `--disk-cache 2` adds the PSF kernel, `0` writes nothing.

## webSMLM

Each release carries `cellfield_block.js`. In the webSMLM repository run
`node tools/sync_cellfield.mjs <path to cellfield_block.js>`.

## Build from source

See `CLAUDE.md` in the repository for the full build and test matrix (core native and WASM, adapter MSBuild,
golden vectors, GPU checks).
