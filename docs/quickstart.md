# Quickstart

## In the browser: the viewer

Nothing to install: the whole simulation runs in your browser as WebAssembly.

<div class="isc-launch" markdown>
[Open the viewer :material-open-in-new:](../../){ .md-button .md-button--primary target="_blank" }

It opens in a new tab and loads about 2.5 MB (under 1 MB compressed); a desktop browser with a mouse works best.
</div>

A first tour, about five minutes:

1. **Look at the sample.** The left half is the sample: an endless field of cells, each with its nucleus,
   microtubules and nuclear pores. Drag to move the stage, scroll to zoom. **Shift-drag** tilts and turns the field
   into 3D; the strip along the bottom is a side view (x-z). **View ▾** (top left) switches layers on and off. The
   dashed square is what the camera sees.
2. **Go live.** The live view on the right starts by itself once the engine is up (**■ Stop** stops it, **▶ Live** goes
   on). Every frame is a simulated camera image of the dashed square: by default
   DNA-PAINT, so single molecules flash on and off as imagers bind. The sample remembers the light it got, so dyes
   bleach where you look: move away and back and the place you imaged is dimmer than its fresh neighbours.
3. **Focus and look closer.** The **Z** field (or Page Up / Page Down, or the wheel over the live image) moves the
   focus through the cells; the side view on the left shows the focal plane. **Ctrl + wheel** zooms into the live
   image; **Display** sets the contrast over a live histogram. On a phone, the live image comes first with the sample as
   an inset (tap it to swap them); two fingers up or down on the live image focus, a pinch zooms, and the **Focus** box
   has a ruler to swipe ([the viewer reference](try-viewer.md) has every gesture).
4. **Switch the microscope.** In **Configuration**, **Mode** changes the experiment: dSTORM, DNA-PAINT, PALM,
   WideField or BrightField, each with its typical dye, lasers and filters. Try **WideField**, then **BrightField**:
   the same cells, three very different pictures. **Drift** makes the sample wander, **Camera** swaps the sCMOS for an
   EMCCD. Under **Illumination & light path** each laser is a slider; click the spectra plot for what reaches the
   camera.
5. **Change the sample.** The settings under the image hold the sample and its labels: the microtubules and the
   nuclear pores each have a mode and a dye (**None** switches a target off). **Specimen** mounts DNA-origami
   **Nanorulers** instead of cells (set the focus to about 0.02 µm, their dyes lie on the glass). **Basic / Advanced /
   Expert** (top right) shows more settings; the search box finds any of them, and every name has a help text on hover (a long press on a phone).
6. **Record a movie.** **● Record** asks for a number of frames, renders them and plays them back. **Save .tif**
   writes a 16-bit TIFF stack that opens in ImageJ/Fiji with its pixel size and frame interval, ready for your SMLM
   or tracking software. The file's header holds every setting, so `insiliscope_cli --spec` reproduces it exactly.
7. **Save your setup.** **Save…** in the Setup box (top of the controls column) writes a `.isc.json` file in parts
   (instrument, settings, sample, measurement with the sample's light history, view); **Load…** takes all of a file
   or only some parts, so the same sample can be seen on another microscope.

Everything on the screen, setting by setting: the [viewer reference](try-viewer.md). What the simulation can show, with
side-by-side movies: the [Gallery](gallery/index.md).

## Micro-Manager

The adapter needs a [Micro-Manager 2.0 nightly build](https://micro-manager.org/Micro-Manager_Nightly_Builds) from 21 February 2026 or later (device interface 75);
older builds refuse the DLL.

1. Download `mmgr_dal_inSiliScope.dll` and the configurations `inSiliScope_Basic.cfg`, `inSiliScope_Advanced.cfg` and
   `inSiliScope_Expert.cfg` from the latest [release](https://github.com/kjamartens/inSiliScope/releases){ data-umami-event="link" data-umami-event-to="releases" } (verify the
   DLL with its `.sha256` file) and copy them into your Micro-Manager folder.
   The PSF (a scalar Gibson-Lanni model with Zernike aberration modes, or a Gaussian) needs nothing else.
2. Start Micro-Manager with `inSiliScope_Basic.cfg`. It loads the hub `inSiliScope` and one device per part of the
   microscope: `Camera`, `XYStage`, `ZStage`, `Objective`, `EmissionPath`, `FilterCube`, `Lasers`, `TransmittedLamp`,
   `SampleHolder`, `CellField`, `Nanorulers`, `Fiducials`, `Fluorophores` and `Renderer`, with the config groups
    - `Channel`: one preset per label mode with the cell field's typical dye (`AF647 dSTORM`, `mEos3.2 PALM`,
      `ATTO655 DNA-PAINT`, `mEGFP WideField`; the nanorulers' docking strands take their own typical dye in that mode)
      and `BrightField`;
    - `Objective`, `Camera` (Kinetix22 with a 0.667x relay: 97.45 nm pixels at 100x; iXon Ultra 897 with 1.6x: 100 nm),
      `Quality` (Fast / Realistic / Exhaustive), `Drift` (Off / Low / Medium / High / Extreme) and `Specimen`
      (`CellField` / `Nanorulers`, the `SampleHolder` position);
    - `Illumination` (`Epi` / `HILO` / `TIRF`: where the laser spot sits in the objective's back focal plane,
      `Lasers.BfpPosition` 0 / 0.95 / 1.03) and `BfpPosition` (the same as an editable field, 0 = epi, 1 = the critical
      angle). The channels' light presets set it too: the DNA-PAINT channel HILO, the nanorulers TIRF, the others epi
      ([Illumination geometry](physics/dyes-and-light-path.md#illumination-geometry));
    - `Laser 561 nm (kW/cm2)` and `Laser 640 nm (kW/cm2)`: the two lines' powers at the sample as editable fields in
      the main window (one property each, `Lasers.Laser561KWcm2` / `Laser640KWcm2`; 0 = off), e.g. 561 alone for the
      nuclear pores, 640 alone for the microtubules;
      **Quality also sets how much of each blink's PSF halo is drawn**: Fast and Realistic leave out pixels below 1e-5
      and 3e-6 of the emitter's photons, Exhaustive draws the whole kernel ([the halo cut](physics/optics.md)); and
      **which dim blinks are skipped**: a blink giving a frame fewer than 100 (Fast) or 50 (Realistic) photons is not
      drawn in it, Exhaustive draws every one;
    - and the pixel-size calibration of every objective and camera.

   `inSiliScope_Advanced.cfg` adds the excitation filter, dichroic and emission filter wheels and the
   channel `mEGFP WideField + BrightField`; `inSiliScope_Expert.cfg` shows every property. The hub's pre-init `Detail`
   (Basic, Advanced, Expert) decides which properties a session shows; to build a configuration of your own, add the
   hub `inSiliScope` in the Hardware Configuration Wizard, then its devices.
3. Take a snapshot of the cell field, move the XY stage, change the Z stage. The Z stage position is the focal plane
   height above the coverslip (0 = coverslip in focus; it starts at 0.5 um). The `Specimen` group (or `SampleHolder`)
   mounts the [DNA-origami nanorulers](samples/nanorulers/index.md) instead: set the Z stage to about 0.02 um (their
   dyes lie 13.5 nm above the glass); the `Nanorulers` device sets their design, density and docking strands per spot
   (Advanced: the bending's persistence length, the landing). Mounting a sample is a new sample: the lit history and
   the drift start afresh. `Fiducials.DensityPerUm2` (e.g. 0.02) puts [fiducial beads](samples/fiducials.md) on
   either sample, `Fiducials.Type` picks the bead.
4. Pick a `Channel`: the label mode and its typical dye for every target; the lasers and the filter cube follow
   (`Lasers.Preset` `Auto`: the presets of the labelled targets combined). The cell field has two targets, imaged
   together: the microtubules (`CellField.Microtubules_*`, ATTO 655 DNA-PAINT at 640 nm) and the nuclear pores
   (`CellField.NuclearPores_*`, Cy3B DNA-PAINT at 561 nm, through a multi-band filter set); the nanorulers have one,
   `Nanorulers.Docking_*`. Each target has its own mode (`_Mode`) and label (`_Label`: Typical, the other dyes with
   data for that mode, or None to leave it unlabelled); custom dyes are `Fluorophores.Dye1..3_*` (Expert). In DNA-PAINT,
   `CellField.Microtubules_ImagerNanoMol` / `Nanorulers.Docking_ImagerNanoMol` (Advanced: the imager concentration in
   the buffer, default 1 nM for the microtubules, 20 nM for the nanorulers) sets how many sites blink per frame: binding events per site per second = k<sub>on</sub>
   × c, from the moment it changes. More imager means denser frames and more free-imager background.
5. The light comes from the shutters: `Lasers` (fluorescence) and `TransmittedLamp` (BrightField). With both open
   (the Utilities `Multi Shutter`, as in the Advanced channel `mEGFP WideField + BrightField`) the camera sums the two;
   with none open it takes dark frames. The autoshutter opens the Core-Shutter for each image.

Every device and property, with its tier and default: [Micro-Manager](micro-manager.md#devices-and-properties). Snaps, live mode and
sequences (also hardware z stacks: the `ZStage` is sequenceable) render live. `Renderer.WriteScopeSpecTo` (Expert)
writes the settings of the next frame to a file that `insiliscope_cli --spec <file>` renders. The camera remembers what
light each place of the sample has had (snaps, live acquisition and sequences add to it): imaging bleaches and uses
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
build/native/cli/insiliscope_cli --specimen Nanorulers --out rulers.tif --frames 1000 --size 64 --exposure-ms 100
build/native/cli/insiliscope_cli --help          # every option and its default
build/native/cli/insiliscope_cli --prepare 1 --disk-cache 2   # warm the caches (world, cells, PSF kernel), no movie
build/native/cli/insiliscope_cli --settings setup.isc.json --out next.tif --frames 100   # the viewer's saved setup
build/native/cli/insiliscope_cli --settings lab.isc.json:instrument --settings mine.isc.json:sample,settings,measurement --out mix.tif
```

`--settings` reads a file from the viewer's **Save…** (all its parts but the view, or those named after a colon;
later files and options override). When its measurement holds a light history, the CLI replays it and writes the live
sample's next frames, the ones the viewer would take next.

With `--drift-xy-speed-nm-per-sec` / `--drift-z-speed-nm-per-sec` the sample drifts (mean speeds; see `--help` for
the direction, its wander and the random-walk jitter `--drift-xy-nm-per-sqrt-sec`) and the CLI writes the true drift per
frame to `movie.drift.csv`.

The CLI keeps the packed cell positions in the same per-user cache directory as the adapter (`--disk-cache 1`, the
default); `--disk-cache 2` adds the PSF kernel, `0` writes nothing.

`--specimen Nanorulers` mounts the DNA-origami nanorulers (their options `--origami-*` and the docking strands' label
`--ruler-*`; without `--z` the focus is on their dyes). The [samples and physics pages](samples/index.md)' figures are
made with the CLI's diagnostic outputs, which change no movie:
the expected photons before the camera (`--photons-out`; with `--history-before` for a sample lit under other settings
before the movie, as Micro-Manager's illumination history keeps it), the PSF kernel and pupil (`--psf-out`), one blink as a movie
splats it (`--splat-out`), the resolved light path and camera (`--setup-json`), dye sites and schedules (`--dyes-json`),
the mean-field dye density (`--density-out`), BrightField phase screens (`--bf-screens-out`) and the preset tables
(`--presets-json`); `--help` lists them. `tools/build_physics_figures.py` turns them into the figures.

## webSMLM

Each release carries `cellfield_block.js`. In the webSMLM repository run
`node tools/sync_cellfield.mjs <path to cellfield_block.js>`.

## Build from source

See [BUILDING.md](https://github.com/kjamartens/inSiliScope/blob/main/BUILDING.md) in the repository for the build
and test steps (core native and WASM, the viewer, the webSMLM block, the Micro-Manager adapter, the docs site).
