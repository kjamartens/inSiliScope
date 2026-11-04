# Changelog

Versions follow semver; while 0.x, any release may change the output for a given seed. Changes that do are marked
**seed**. The development log before the first release is in [docs/dev/history.md](docs/dev/history.md).

## Unreleased

- **Ground truth** ([docs](docs/ground-truth.md)): a recording's **Save** (was Save .tif) opens a box: the movie alone
  (.tif), or with its instrument settings (a settings file taken as the Record began, which reproduces the movie with
  `insiliscope_cli --settings`) and its ground truth in one .zip. The ground truth: a table per frame (time, stage,
  focus, drift, pattern state); a localization table of the blinks with at least 300 photons (the box sets it) in the 500 × 500 nm square around
  them, from the PSF at its own sampling (the same at any pixel size), with the
  dye's position, its binding site, photons detected / on the camera / emitted, the PSF-weighted background, the beads
  as `fiducial` rows; for a recording without a blinking label (WideField) a dye list instead; for continuous dyes
  photon-weighted density stacks (a target each, all on one grid, detected photons per 10 nm voxel or as fine as
  fits 1 GB, 8-bit with the scale as Fiji's calibration, deflated, each slice's z as its label, voxel size and world
  origin for Fiji, voxel size for napari; one C++ voxelizer, `Simulation/Voxelize.*`, also voxelize.html's)
  with `voxelize.html` (☰ › Tools, and in the zip beside a dye list), a converter that needs no installation, reads
  the saved zip itself, estimates the compressed size and makes the stack plane by plane, a compressed TIFF that
  Fiji's Import › TIFF Virtual Stack opens without loading it, with each slice's z in its title line (or a .zip
  holding an uncompressed TIFF); 8, 16 or 32 bits, one stack or a channel per target,
  compressed). Every saved zip opens in napari through its `open_in_napari.py`, dropped on napari's window: the movie, the density
  stacks and the localizations per frame, all in world coordinates. The ground truth is made at Save on a worker of its own, with its progress shown; a 128 px WideField
  recording beside a nucleus (4 million dyes) fits in the viewer's memory. The cli writes the same files: `--gt-out`, `--gt-min`, `--gt-volume`. Core ABI 19
  (`isc_dye_details_in_window`); every output unchanged.

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
