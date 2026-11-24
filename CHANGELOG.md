# Changelog

Versions follow semver; while 0.x, any release may change the output for a given seed. Changes that do are marked
**seed**. The development log before the first release is in [docs/dev/history.md](docs/dev/history.md).

## Unreleased (changes since v0.1.0-rc23)

- **Viewer: the Microscope view keeps up under BFP light maps.** Its panels waited on the movie worker behind a
  setup readout per settings step, each of which made the maps' light again (~10-15 s there): five steps of the BFP
  slider put the panels about a minute behind (the live image, on its own worker, kept up). Setup readouts now go
  one at a time and a queued one for settings that have since changed is dropped: the same drag 60 -> 33 s, and a
  longer drag no longer adds to it.
- **Viewer: a remote Linux server does the computing** ([docs/remote.md](docs/remote.md)). ☰ › Remote server… gives
  one `ssh` command; run in a terminal, it fetches the viewer's own version of the new `insiliscope_server` into the
  home directory on the server (no admin rights; `tools/remote.sh`, fetched from the repository at the viewer's release tag) and tunnels it to the page, which
  connects by itself. Live (by default), Records, movies and ground truth then run there, byte for byte the frames
  the browser makes; when the server goes, the viewer goes on in the browser, the live sample where it was. Clusters:
  "Run through" starts it in a scheduler's job (`srun ...`). Release asset `insiliscope-server-linux-x86_64.tar.gz`
  (glibc 2.28+, x86-64). Fixed on the way: a Record started while a settings file's light history was replayed never
  began.
- **Remote server: its GPU** ([docs/remote.md](docs/remote.md#gpu)). A server with a GPU (NVIDIA, AMD or Intel, with
  its Vulkan driver) uses it by itself: the WideField dye planes' images (also each SIM state's), and in Records and
  movies the blinks' PSFs and the camera noise. The same WGSL kernels as the viewer's WebGPU path, through
  wgpu-native (shipped in the server's tarball); frames within float rounding of the CPU's (about 1 pixel in 100 a
  count apart). `--gpu off` (or `ISC_GPU=off`) keeps the CPU; the cli takes `--gpu auto` (off by default).
- **Viewer: Records and movies splat their blinks and draw the camera noise on the GPU** (WebGPU: Chrome, Edge), with
  the remote server's shader. Frames within float rounding of the CPU's (about 3 samples in 10000 a count apart); a
  256 x 256 px, 40-frame DNA-PAINT movie 0.69 -> 0.16 s on an Intel Iris Xe. The engine's movie stays synchronous: the
  GPU's frames come back through a canvas. `?gpu=0` keeps the CPU (the frames then equal `insiliscope_cli`'s byte for
  byte), `?wfgpu=0` only the WideField images. Live frames, ground truth and BrightField stay on the CPU.

Large recordings:

- **Viewer Record, frames to disk:** "write frames to disk" (in the Record box, on by default) writes the frames and the
  ground truth's files into the browser's own disk storage as they are made, so a recording is bounded by the disk
  space the browser gives the page, not the tab's memory; Save makes the .tif or .zip from those files.
- **Viewer Record, straight to a file (Chrome, Edge):** "save to a file as recorded" asks for a .tif at Start and writes
  the frames into it as they are made, with no storage quota; the TIFF's tags are written in front of them at the end.
- **Long Records in strips no longer run out of memory:** a strip's height now counts the movie's blinks (they grow with
  its length), the cached dyes of one strip are let go before the next, and a movie keeps one copy of its blinks instead
  of up to four (a 1600 px, 1000-frame DNA-PAINT Record stopped with "out of memory" about a quarter of the way through).
  A strip no longer holds all its fluorophores at once either: they are made as the strip is walked and let go after
  (a 1600 px Record over dense cells, or a 5000 px one, ran out of memory in its first strip). Core ABI 21
  (`isc_world_set_dye_cache_hard`); the webSMLM block's ABI is 21 (sync it into webSMLM).
- **Movies in strips:** a movie whose field does not fit in memory, or has a side past 4096 px (up to 16384), renders
  in strips of rows (all frames of one strip, then the next). The frames are those of the field rendered whole (blinks,
  beads and the camera noise to the last bit; WideField and BrightField within rounding). cli `--strip-rows n`
  (0: when needed, -1: never); `--estimate` reports the strips. A whole movie is unchanged.
- **Viewer Record, a field of its own:** the Record box's field (W x H px, empty = the live field), centred on the
  stage; the world view outlines it while the box is open. The illumination beam scales with the field (Beam FWHM is a
  share of the field), which the box says before Start. Its settings file reproduces it in the cli.

## v0.1.0-rc23 (changes since v0.1.0-rc22)

Any light in the objective's back focal plane; the viewer's settings as a Micro-Manager configuration; large movies:

- **BFP light maps.** The excitation can be any distribution over the pupil, written as equations (intensity, phase,
  polarization, entry tilt, focusing, coherence, timing and scans over the pupil and the exposure) or read from images
  (viewer: PNG or TIFF; cli and Micro-Manager: TIFF). Coherent light is computed as a numerical field (a focused beam
  has its waist, a sheet its Rayleigh range), incoherent light point by point; the power through the pupil stays the
  laser's. Default outputs are unchanged.
- **Two presets on the Pattern select** (Micro-Manager `Lasers.Pattern`, cli `sim-type`): **LightSheet** (an inclined
  light sheet, `SheetThicknessUm` x `SheetWidthUm`) and **RingTIRF** (the TIRF spot turned once round per exposure),
  plus **Maps** (the equations alone, Expert: `Lasers.BfpMaps`, `BfpSlots`, `BfpCycleFrames`).
- **Viewer:** the equations' box with 15 examples (a light sheet, ring TIRF, Bessel beam, radially and azimuthally
  polarized rings, a line scan, SIM2D, lattices and more from images in `bfp_maps/`), map images, settings files that
  carry the images; the Microscope view's Illumination profile follows the focus (its height moves with Z, the focus
  dashed and marked on the slider); the Back focal plane panel draws the pupil at 512 x 512 (Expert: BFP drawing).
- **One sampling setting** for the maps' light: `Renderer.BfpSamplesPerPeriod` (cli/viewer `bfp-samples-per-period`;
  Quality Fast 6, Realistic 12, Exhaustive 24) samples per the light's finest period -- a finer tile over the field of
  view, z per axial beat and evanescent decay, and a finer pupil where lit light meets a hard edge (RingTIRF's spot under
  the pupil's rim: 1 % -> 0.1 % at Realistic). A tile too fine for the memory is made coarser and the BFP panel says so.
- A focused coherent beam over a large part of the pupil takes seconds natively and up to a minute or more in the
  viewer to make (once per light).
- **Viewer: a Micro-Manager .cfg of its settings** (☰ › Micro-Manager .cfg…): the Expert configuration with a Startup
  preset that sets every property to the viewer's value. Every viewer setting now has a Micro-Manager property (new
  Expert rows: the cell shape, per-target dye fields, the custom dye slots' spectra and pre state, BrightField); a
  browser check fails on a viewer setting without one.
- **`tools/isc_render.py`** (a window or a command line; `tools/build_isc_render_exe.py` makes a one-file Windows
  exe): movies from a saved `.isc.json` or `.cfg` at a list of positions to TIFF, with an estimate of disk and memory
  first, a progress bar per position, optional ground truth, and an unfinished movie named as such.
- **Large movies:** the cli writes a BigTIFF past 4 GB (64-bit file positions; a file past 2 GB was broken on
  Windows), refuses a movie the disk cannot hold and says out of memory in words; the viewer's Records stream their
  frames out of the engine (no 1 GB cap) and save a BigTIFF past 4 GB.

## v0.1.0-rc22 (changes since v0.1.0-rc21)

Micro-Manager camera sizes up to 4096 x 4096 px, any width and height, with a memory check before anything renders:

- **Micro-Manager: any camera size up to 4096 x 4096 px.** The pre-init `Camera.FovSize` (four square sizes up to
  1024) is replaced by `Camera.SensorWidthPx` and `Camera.SensorHeightPx` (16..4096 each, default 256 x 256), set any
  time but during an acquisition. Configurations saved with `FovSize` must drop that line (the shipped ones did).
- **A size too large for the PC's memory is refused before it renders**, with the reason: the estimated memory, its
  largest parts and the largest square that fits (e.g. the default scene at 4096 x 4096 needs ~31-44 GB). A snap, a
  sequence or the live view is refused the same way when a later change (more labelled targets, the lasers opened)
  makes the setup too large. Large fields stay slow: see spec/ALGORITHM.md "Large fields of view".
- cli / settings files: `height` (0 = square), `size` up to 4096 (was 2048). On a non-square field the beam's
  `illum-fwhm` is a share of the longer side.

## v0.1.0-rc21 (changes since v0.1.0-rc20-hotfix1)

Live stage and focus moves without the lag, and a roomier live view:

- **Stage moves in live are faster** (the viewer's WebAssembly, 128 px, a 1 um step; every frame the same as before):
  - WideField: 110-250 -> 35-45 ms a move. By the nucleus edge, where the pores are drawn dye by dye beside the
    mean-field microtubules, a move took 3-7 s and every frame after it ~1 s (the microtubules' 2.1 M dye windows were
    fetched and unused); now ~1 s once, then ~5 ms.
  - The SIM imaging mode: ~400-450 -> ~160-230 ms a move (threaded viewer).
- **Focus steps in live are faster:**
  - WideField: the first step 1.25 s -> 83 ms, later ones ~70 -> ~57 ms (every frame the same as before).
  - SIM: 200-400 -> 110-165 ms a step (threaded viewer), no longer slower the longer you scrub: the light history's map
    sums each kind of light's recent records into one grid for dyes that bleach (dSTORM, WideField), one term per
    beam instead of one per beam and focus position visited. Frames unchanged.
  - **seed** A fine focus scrub under HILO (SIM and WideField): the light history records the sheet's height on the dye
    planes' grid (100 nm, or 25 nm at Exhaustive quality), so focus positions between two levels share one record.
    SIM2D 0.02 um steps 1.4 -> 0.8 s a frame (single-thread viewer). Frames change where foci merge (SIM up to 0.3 %,
    WideField 0.01 % of the signal); a focus on the grid (the default 0.5 um, 0.1 um steps) is unchanged.
- **Live WideField no longer pauses every ~14 s** (~100-170 ms) when its bleaching approximation is renewed: the next
  one is made a few planes per frame beforehand. Frames are within 1e-4 of the signal of before.
- **Viewer layout:** the settings column starts folded at every launch, and folding it gives its room to the live
  image (the world view keeps its size); the live panel takes 41.6 % of the window by default (was 50 %).
- Core ABI 20 (`isc_continuous_in_map_masked`); the webSMLM block's ABI is 20 (sync it into webSMLM).

## v0.1.0-rc20-hotfix1 (changes since v0.1.0-rc20)

Viewer fixes and a SIM imaging mode:

- **Live keeps up with the exposure.** It was slower than one frame per exposure: 50 ms gave about 16 fps, 10 ms about 20 fps even with 10 ms frames.
  - The canvases are drawn at the display's refresh. Drawn straight from the worker's message, Chrome's canvas rate limiter held the page ~150-280 ms several times a second.
  - Frames follow an absolute schedule (each due one exposure after the last one's due time), and the next one is asked for just early enough to arrive by then.
  - The world view is redrawn after a live frame only when the drift moved it (in Firefox each redraw held the page ~240 ms).
  - Measured, 64 px at 10 ms: Chrome 19 -> 76 fps, Firefox 96 fps; at 50 ms: 20 fps.
- **SIM imaging mode** (Micro-Manager Channel group and the viewer's Imaging mode): the WideField labels under a SIM2D pattern at BFP position 0.75, 50 nm pixels (`EmissionPath.EmissionMagnification` 1.3 with the Kinetix22 at 100x) and 100 ms frames. The other modes turn the pattern off and leave the rest.
- **Layout:**
  - The top bar: ☰, save and load, then Sample | Microscope at the left; the name centred; the settings level at the right.
  - The Specimen select is the first row of Acquisition.
  - Live / Snap / Record stay at the top of the controls while they scroll.
  - The Info box keeps one line per row and room for its warning line, so nothing under it moves; several warnings show side by side by short names.
  - The settings column's tab reads "Show specimen settings"; its fold button is an arrow-to-bar icon in the accent colour.
  - The ☰ menu is tidier, and About names the release and commit the site was built from (`build_info.js`, written by `tools/build_site.sh`).

## v0.1.0-rc20 (changes since v0.1.0-rc19)

Faster live structured illumination with WideField labels, and a multithreaded viewer:

- **The viewer renders on several cores.** On first visit the page installs a small service worker (`coi_sw.js`). It adds two headers to the viewer page only and reloads the page once. The page is then cross-origin isolated, and live runs a threaded build of the engine.
  - Live uses half your logical cores less one, at most 8 (5 on a 12-thread CPU), so a core stays free for the computer and one for the page. `?threads=N` sets the number, `?threads=0` runs on one core.
  - Frames are the same bit for bit as on one core.
  - Where isolation is not possible (some private windows), a notice at the bottom of the window says the viewer runs on one thread.
  - The usage counts are sent by the page itself there, since Umami's script cannot load (the same data; [privacy](docs/privacy.md)).
- **Live SIM + WideField** keeps each pattern state's bleach basis across the cycles and makes the next one ahead, one image per frame, never a pause. Native, one thread, SIM2D at 256 px: about 270-400 ms a frame before, 112 ms now on average (40 ms steady). The first ~100 frames cost about twice as much while the bases are built. Live frames differ from a recording's exact ones within the basis's tolerance (a few ADU) **seed** (live only; `fresh-basis 1` keeps the old path).
- **WideField dye planes are 100 nm by default** (`Renderer.WideFieldZPlaneNm`, `wf-plane-nm`; Quality Fast and Realistic; Exhaustive keeps 25 nm, the old default). Each plane is an FFT per image, so WideField frames are much faster (live SIM2D 2.3-2.6x). The photons move by 0.25-0.9 % rms, far below the shot noise. Under TIRF the planes stay at most a sixth of the light's decay depth (about 26 nm), whatever the setting. See [Dye plane thickness](docs/physics/optics.md#dye-plane-thickness) **seed** (WideField and mean-field images).
- A pattern's light is made once per dye plane instead of once per cell (LatticeSIM3D about 40 % faster a frame; output unchanged).
- Tools: `live_bench` (live frames timed, cli build), `tests/web/live_speed.mjs` (the viewer module under Node), `tools/fft_bench` (FFT libraries compared: no switch).

## v0.1.0-rc19 (changes since v0.1.0-rc18)

Viewer redesign ([design doc](docs/dev/DESIGN.md)):

- **Themes and design tokens:** Dark, Light or Follow system, plus a density setting (Comfortable / Compact), all in ☰. Colours and sizes come from one set of design tokens (`web/design/tokens/`).
- **New layout:**
  - **Acquire** sits under the live image: Live / Snap / Record, a stage joystick and focus wheel, the Info box, the display and Acquisition.
  - **The instrument** (light path, objective & PSF, camera) sits in a second column. A handle spreads it over the image.
  - **The settings** are in a column at the right edge that folds to a tab. It has one search and a "Changed only" filter.
  - Each setting appears once, with a reset per row and per group.
- **The display** is an upright level bar beside the stage, with a grey scale, a histogram and a handle per level. On a phone it lies down.
- **The Info box** contains:
  - laser on/off buttons;
  - the setup in four rows: labels, light, optics, camera;
  - warnings when something is off, each with what to do. It warns about no light, nothing labelled, a label no line excites or the filters block, saturation, the focus off the sample or above the TIRF light, no cell in view, and slow frames.
- **The browser tab** is titled inSiliScope.

Behaviour:

- **The BFP position** (viewer and Micro-Manager) moves only by:
  - its slider;
  - the light preset (`Lasers.Preset`);
  - Epi / HILO / TIRF;
  - a pattern pick;
  - a reset or a settings file.

  A dye, mode (channel) or slide-dye pick, the imaging mode and a specimen switch now leave it as set. They used to apply the preset's position.
- **Viewer, SIM at the BFP's centre:** with a SIM pattern on, a BFP position below 0.05 switches the pattern off (epi). Raising it to 0.05 or above brings the pattern back.

Benchmarks and site:

- Benchmarks: each Micro-Manager live case's rate is the median of three 1.5 s windows (one window let a single stall set the rate: rc18's mEGFP WideField dip at 10 and 50 ms was one, not a slowdown).
- Site: a release's Benchmarks entry shows up again: the push that merges a release no longer deploys the site before the release does (rc18's entry was missing).

## v0.1.0-rc18 (changes since v0.1.0-rc17)

- **Ground truth** ([docs](docs/ground-truth.md)): a recording's **Save** can add its settings file (reproduces the
  movie) and its ground truth in one .zip: a table per frame; a localization table (blinks with at least 300 photons
  in the 500 nm square around them, with positions, binding sites, photons and background); for WideField labels a
  dye list; density stacks of the continuous dyes. `voxelize.html` (☰ › Tools) makes stacks at any voxel size;
  `open_in_napari.py` opens the recording in napari. The cli writes the same: `--gt-out`, `--gt-min`, `--gt-volume`.
  Core ABI 19 (`isc_dye_details_in_window`); every output unchanged.
- The ground truth is computed while the Save box is open, or alongside the Record when the last Save included it
  (a Record up to ~10 % slower; Live is never slowed).
- A Record's settings file reproduces it (its settings as it began, the recorded runs); one started before the
  viewer had loaded its option list was empty.

## v0.1.0-rc17 (changes since v0.1.0-rc16)

New samples and illumination:

- **Bead sample** (a third specimen, `Beads`): fiducial beads alone, no cells or dyes, mounted at 0.3 beads/µm² with
  clumping (`Fiducials.AggregationPct`, 40 % on this sample, 0 elsewhere = the old field) and its own light preset
  (561 nm at 0.1 kW/cm²). Beads of any sample: a size apart from the type (20 nm beads), the beads' own PSF range
  (`Fiducials.PsfZRangeUm`, 14 µm) and no bead dropped by depth any more (beyond the stack a growing Gaussian,
  `Fiducials.FarDefocus`) **seed** (beads only); faster drawing through a gel layer, a clump drawn as one bead of its
  volume (`Renderer.BeadClumpsAsOne`; Quality Exhaustive: each bead).
- **Fluorescent slide** (a fourth specimen, `Slide`): a uniform dye layer on the coverslip (thickness, dye or the
  TetraSpeck mix, concentration, bleach half-time), its light the collected column of the beam, bleaching per pixel
  column from the sample's light history; light presets Slide-405/488/561/640.
- **Structured illumination (SIM)**: the light as coherent beams in the back focal plane (`Lasers.Pattern`: SIM2D,
  SIM3D, LatticeSIM2D, LatticeSIM3D, Custom beam matrix; polarization, phases, angles, centre amplitude, phase drift,
  the axial anchor `Lasers.PatternAnchor`), the pattern stepping with every frame; the kinetics on the cycle mean, the
  emission with each frame's pattern (blinks, mean field per pattern state, background, beads over their volume, the
  slide's fringes through the PSF). Micro-Manager: each frame's pattern index in its metadata
  (`inSiliScope-PatternFrame`), `PatternRestart`, `PatternFrame` / `PatternState` / `PatternPeriodNm`. Under a pattern
  dSTORM's initial-ON and PALM's pre-state dyes glow under the cycle mean (`Renderer.PatternOnContinuousDyes`, Quality
  Exhaustive: each frame's pattern). Off = the previous output bit for bit.
- The slide and the beads read their past light by chapter of the light history (bit for bit under one unchanged
  light) **seed** (beads after a change of light).

Speed and memory:

- **PSF kernel sample spacing** (`Renderer.PsfMaxSampleNm`, cli/viewer `psf-max-sample-nm`; Quality Fast 30 nm,
  Realistic 15 nm, Exhaustive no limit): a small camera pixel no longer multiplies the kernel's samples. At a 20 nm
  pixel 2 samples per pixel (10 nm) instead of 6: 128 px DNA-PAINT SIM2D 8.9 s / 3.5 GB -> 1.5 s / 392 MB, image error
  ~0.1 %. Default pixels unchanged; **seed** at pixels below ~90 nm.
- **SIM Records of mean-field labels**: a pattern state that a movie draws in fewer than 12 frames makes no bleach
  basis; each of its frames is one image at its exact weights. A 20-frame SIM2D Record of a WideField label: 20.5 s ->
  3.1 s in the viewer (the frames within 6e-5 of the old ones) **seed** (SIM movies of WideField labels, slightly).
- RAM: a PSF stack's splat sums made on first use in one layout (two thirds less for a WideField label); a SIM state's
  mean-field spectra streamed; the slide's fringes per batch; the viewer module's heap up to 4 GB (was 2 GB). An
  out-of-memory movie worker is replaced alone and its jobs answered. A memory report per cache (cli `ISC_TIMING=1`,
  the viewer's Expert performance box).
- Live SIM frames of blinking labels ~3x faster (the frame counter out of the cache keys).

Viewer:

- **Microscope view** (Sample | Microscope in the title bar): the light path drawn with its beams, and tiles for the
  illumination in the sample, the PSF, the back focal plane and the spectra, described by the C++ (replaces Preview
  illumination, Preview PSF and the spectra plot); zoomable images, pop-ups on parts and beams.
- Record starts at a time set in its box (default 0 s, was the Start time setting, 60 s); the live info line shows fps
  and ms/frame only (the rest in its tooltip) and no longer moves the bar's buttons.
- The world view keeps its splitter width over a reload; Preview illumination reopened after a close (before the
  Microscope view replaced it).

## Unreleased (0.1.0, first public release)

A simulated fluorescence microscope around one world model of a sample, the same in every target.

- **Samples**: a field of adherent cells (cell body, shaped nucleus, cytoplasm, microtubules, nuclear pores; refractive
  index for brightfield) DNA-origami nanorulers (rods and a 2D origami), a bead sample and a fluorescent slide, each a pure function of the seed and the
  position, endless in x and y. Optional fiducial beads on either sample (gold, TetraSpeck, FluoSpheres, nanodiamonds;
  spectra and brightness after Balinovic et al. 2019), on the coverslip, on the sample or in a gel layer, slowly
  bleaching, a defocused bead's light spread over the whole frame.
- **Labels and photophysics**: per target a dye from a library of FPbase spectra in dSTORM, PALM, DNA-PAINT or
  WideField mode, a label linkage (antibodies, nanobody, SNAP/Halo), the illumination history of every place (bleaching
  where you looked), DNA-PAINT binding kinetics and the free imager's background.
- **Light path**: lasers, excitation, dichroic and emission filters, the camera's QE curve; the beam in the back focal
  plane (epi, HILO, TIRF) with a Gaussian, FlatTop or flat cross-section (default Gaussian, FWHM the width of the
  field of view); structured illumination (SIM2D, SIM3D, lattice SIM, a custom beam matrix) stepping with every frame.
- **Optics and camera**: a scalar Gibson-Lanni PSF with Zernike aberration modes (aberration presets, double helix)
  or a Gaussian PSF; sCMOS (Kinetix22) and EMCCD (iXon Ultra 897)
  noise; sample drift; brightfield by multislice propagation through the cells.
- **Browser viewer** (the site's landing page): pan, zoom and tilt through the sample in 3D (microtubules, nuclear pores,
  nuclei, dyes; looks with depth colours), a live microscope panel driven by the Micro-Manager configurations
  (Configuration, illumination, objective, camera with its magnification, focus, record to TIFF), a Microscope view
  of the light path, the illumination, the PSF, the back focal plane and the spectra, one seed for the
  world and the noise as in Micro-Manager.
- **Usage counts**: the viewer and the docs count anonymous usage with Umami, without cookies or personal data
  ([Privacy](docs/privacy.md)); nothing with Do Not Track or Global Privacy Control. The adapter and the cli send nothing.
- **Micro-Manager adapter** (`mmgr_dal_inSiliScope.dll`, Windows x64): a hub with one device per part of the
  microscope, properties in Basic / Advanced / Expert tiers, shipped configurations, live mode, sequences and hardware z
  stacks, GPU rendering (Direct3D 11).
- **cli** (`insiliscope_cli`): headless TIFF movies of any setting, reproducible from a Micro-Manager frame
  (`Renderer.WriteScopeSpecTo` + `--spec`).
- **webSMLM block** (`cellfield_block.js`): the same world model for [webSMLM](https://github.com/kjamartens/webSMLM).
