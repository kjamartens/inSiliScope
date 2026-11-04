# Ground truth

A simulated movie comes with what made it. After a **● Record**, the viewer's **Save** offers three things:

- the **movie** (a 16-bit TIFF stack);
- its **instrument settings** (`settings.isc.json`, which reproduces the movie);
- its **ground truth** (`gt/`): where every fluorophore was, frame by frame, and how much light it gave.

The movie alone saves a `.tif`. With anything else, everything goes in one `.zip`. The command line writes the same
ground truth:

```bash
insiliscope_cli --settings settings.isc.json --out movie.tif --gt-out gt
```

## How it is made

The ground truth is computed when you save, not while recording. The viewer renders the recording again from its
settings file with the ground truth switched on, on a worker of its own (a fresh WebAssembly heap, freed after; live
pauses meanwhile). It starts as soon as the Save box opens with Ground truth ticked, so most of it is done while you
choose the name and options; lowering a threshold or ticking the dye list starts it again, closing the box stops it.
This pass skips what only the pixels need (the camera noise, the transmitted lamp's image). It takes a few times the
recording's own time: a 128 px WideField recording beside a nucleus (4 million dyes) about a minute, with the dye
list. The Save box shows the stage (preparing the sample and the PSF, the frames with their %, writing the files), a
bar and the seconds. (Collecting it during the Record would add ~70 % to every recording, saved or not.)

The settings file is taken as the Record began. It reproduces the frames bit for bit:

- A **fresh** Record is a movie from its start time.
- A Record that **continued the live sample** replays the light the sample had before, then renders the recorded
  chunks as they were rendered.

The one exception is a fresh Record whose mean field ran on WebGPU (WideField labels): those frames can differ from the
CPU's in the last bits, while the ground truth is the CPU's. The Save box says when this applies.

Every number is an expected value, before shot noise and camera noise: the photons the model put into the image, not a
noisy count.

## The files

| File | When | What |
|---|---|---|
| `movie.tif` | always | the frames, 16-bit, ImageJ (pixel size, frame interval, the engine spec in the header) |
| `settings.isc.json` | Settings file | the microscope, sample and acquisition as the Record began, with the light the sample had seen before ([settings files](try-viewer.md)) |
| `gt/frames.csv` | Ground truth | one row per image |
| `gt/localizations_min5.csv`, `_min300.csv` | a blinking label (dSTORM, PALM, DNA-PAINT) | every blink drawn (and every bead) with at least 5 or 300 photons on the camera, one row per frame |
| `gt/fiducials.csv` | beads, no blinking label | every bead per frame |
| `gt/dyes.csv` | Dye list (no blinking label, WideField) | one row per dye |
| `gt/voxelize.html` | Dye list | a converter from `dyes.csv` to a stack at any voxel size (below) |
| `gt/gt_volume_<target>[_<state>].tif` | continuous dyes | a density stack per target, all on one grid: detected photons per voxel |
| `gt/README.txt` | Ground truth | what is in `gt/` |
| `README.txt` | any of the above | what is in the zip |
| `open_in_napari.py`, `__main__.py` | any of the above | open the recording in napari: drop `open_in_napari.py` on napari's window, or `python <name>.zip` ([below](#open-it-in-napari)) |

### frames.csv

One row per image, so a recording that later combines time with z planes or stage positions needs no other format.

| Column | Meaning |
|---|---|
| `frame` | the image's index from 0 |
| `t_start_s`, `exposure_s` | the sample's time at the frame's start and the exposure (s) |
| `stage_x_um`, `stage_y_um` | the field of view's centre (world µm) |
| `focus_um`, `focus_world_um` | the focus as set, and the focal plane's height above the coverslip with the drift in |
| `drift_x_nm`, `drift_y_nm`, `drift_z_nm` | the sample's drift at the frame |
| `pattern_angle`, `pattern_phase` | only under structured illumination: the frame's pattern state |

### Localization tables

One row per blink per frame. The two tables differ only in their threshold: the photons that **land on the camera**,
meaning the part of the blink's PSF inside the image (the pop-up sets both thresholds). The column names follow
ThunderSTORM where it has one.

| Column | Meaning |
|---|---|
| `frame`, `t_s` | the frame and its start time |
| `kind` | `emitter`, or `fiducial` for a bead |
| `id`, `target`, `dye`, `mode`, `state` | the dye (its id is stable across frames and recordings) and its label |
| `x [nm]`, `y [nm]` | position on the camera, nm from the image's top-left corner, drift included |
| `z [nm]` | height above the focal plane |
| `x/y/z_world_um` | position in the sample (µm, z above the coverslip), without drift |
| `site_x/y/z_world_um` | the binding site the dye hangs from (the microtubule's surface, the pore's Nup96 site, the lamina point; a docking strand is its own site). The dye minus the site is the linkage error. |
| `on_fraction` | the part of the frame the blink was on |
| `photons_detected` | photons the blink sent to the camera, if the camera were infinite |
| `intensity [photon]` | those landing on the camera |
| `photons_emitted` | photons it emitted (detected / collection x light path x QE) |
| `offset [photon]` | the other light per pixel under this blink's PSF, weighted by the PSF: Σ w b / Σ w over the pixels it draws. This is everything except the blinks: the background, the free imager, continuous dyes, beads, the slide. |
| `bg_centre_photons` | that light in the centre pixel alone |
| `wavelength_nm`, `excitation` | the emission's wavelength (the PSF's) and the excitation factor at the dye |
| `regime` | `splat`, `binned` (approximate SMLM at high density) or `bead` |
| `bead_diameter_nm` | a bead's diameter |
| `orient_polar_deg`, `orient_azimuth_deg` | the dye's mean emission dipole, if its label fixes one (see below) |

A blink that **Quality** left out of the image is left out of the tables too. At Fast and Realistic, blinks under 100 or
50 photons are not drawn, so in practice the ≥ 5 table starts there. Quality Exhaustive draws every blink.

### Continuous dyes: dyes.csv and the density stacks

WideField dyes, dSTORM's initial-ON phase and PALM's pre-state shine together rather than as separate blinks, so a
localization table makes no sense for them. A recording's ground truth is its localizations or its dye list, never
both: with a blinking label (dSTORM, PALM, DNA-PAINT) the localization tables, without one (WideField) the dye list.
The density stacks come with either.

- `dyes.csv` (WideField only) lists each dye with its position and binding site, its window in the sample's time (`t_on_s`, `t_off_s`:
  when it bleaches or switches), its detected rate now (`photons_per_s`, 0 when no light reaches it) and its photons
  over the recorded frames (`photons_detected_total`, `photons_emitted_total`). A 128 px WideField recording lists
  0.8 to 4 million dyes (~120 to 600 MB), so the dye list is optional; the stacks stay.
- `gt_volume_<target>.tif` sums `photons_detected_total` per voxel: the light each place sent to the camera, not just
  whether a dye was there. Every target's stack lies on **one grid**, the box of all their dyes, so the stacks overlap
  when opened together (in Fiji, Image › Color › Merge Channels; in napari, as layers).
  - The voxels are 10 × 10 × 10 nm, or as fine as fits 1 GB in all uncompressed (one factor for x, y and z, in steps
    of 0.5 nm; the README says so). A 128 px WideField recording beside a nucleus (4 million dyes) gets 12.5 nm
    voxels: two stacks of 1318 × 1309 × 297, 6.3 MB together on disk.
  - 8 bits, scaled to the stack's largest voxel: photons = value × the scale (`insiliscope.scale` in the
    description, and Fiji's calibration, so Fiji shows photons). A voxel is within half a step of its photons.
  - The planes are deflated (zlib; a mostly empty plane to ~1/1000 of its size). Fiji's File › Import › TIFF
    Virtual Stack opens a stack without loading it and shows each slice's z (its label, e.g. `z = 1.1938 µm`) in the
    title line; dragged in, Fiji loads it whole and shows no labels (ImageJ reads none from a compressed TIFF).
  - Fiji/ImageJ and napari read the voxel size: X/YResolution in pixels per µm, `spacing` and `unit=micron` in the
    ImageJ description.
  - Fiji also reads the world origin (`xorigin`, `yorigin`, `zorigin`): its status bar shows each voxel's world x,
    y and z in µm (z at the slice's centre).
  - The world position of voxel (0, 0, 0)'s corner is also in the description (`insiliscope.origin-um`) and in
    `gt/README.txt`. Voxel (i, j, k)'s centre is that origin plus ((i, j, k) + ½) × the voxel size.
  - The stacks are made by the same code as voxelize.html's (below, `Simulation/Voxelize.cpp`): voxelize on
    `dyes.csv` with the same choices gives the same stack, up to the CSV's rounding of positions and photons.

### Other voxel sizes: voxelize.html

The viewer's ☰ › Tools › **Voxelize ground truth** opens it
([online](https://kjamartens.github.io/inSiliScope/voxelize.html)); a zip with the dye list carries a copy,
`gt/voxelize.html`, which opens in any browser, offline, with nothing installed. Drop the saved recording's `.zip` on
it (it reads the table out of the zip and lets you pick one when there are several), or `dyes.csv` or a localization
table, then choose:

- the dyes' positions or their binding sites;
- the value per voxel: detected photons (the default), emitted photons, or a count;
- a target, or all of them as **a channel each** (the default: a composite stack for Fiji) or **one stack** (their
  sum);
- the voxel size (default 10 × 10 × 10 nm);
- the bit depth: **8-bit** (the default), 16-bit, or 32-bit float. 8 and 16 bits scale each channel to its maximum;
  the photons per step are in the description (`insiliscope.scale`), and a single-channel stack carries them as its
  calibration, so Fiji measures photons;
- the file:
  - **a compressed .tif** (the default, also when you dropped a zip). In Fiji, open it with **File › Import › TIFF
    Virtual Stack**: Fiji reads each plane only when shown (a 2 GB stack opens in under a second, in ~120 MB of
    memory), as a hyperstack with a channel per target, and the window's title line shows each slice's z in µm
    ("Nuclear pores, z = 1.9450 µm"). Dragged in, Fiji loads it whole and shows a voxel's world z only in its status
    bar: ImageJ (1.53 and 1.54 checked) reads no slice labels from a compressed TIFF opened that way. napari and
    tifffile read it too.
  - **a .zip holding an uncompressed .tif**: Fiji opens the .zip directly, whole, with the slice labels.
  - **an uncompressed .tif**, with the labels, at full size.

  A mostly empty stack shrinks ~100 times. The page estimates the compressed size before you make it, by deflating a
  few sample planes; on the stacks tried it came within 10 %. The uncompressed size is what Fiji holds in memory when
  it opens the stack whole. The .zip's TIFF and the uncompressed .tif stay under 4 GB (a classic TIFF's limit); the
  compressed .tif can describe a larger stack.

It reads the table as it streams in (a 120 MB `dyes.csv` in under a second) and makes the stack one z plane at a
time, each plane deflated with the encoder of the ground truth's density stacks. That 120 MB `dyes.csv` (776 000 dyes)
at 10 nm voxels (1469 × 1297 × 364 voxels × 2 channels, 1.4 GB uncompressed) takes about 1 s in Chrome and gives a
3.3 MB file; a 2 GB stack opens in ImageJ as a virtual stack in
0.7 s.

## Open it in napari

Every saved zip carries `open_in_napari.py`, a small Python program (and the same as `__main__.py`, which Python runs
when given the zip). With napari installed (`pip install "napari[all]" tifffile`), either

- **drop `open_in_napari.py` on napari's window** (napari runs a dropped `.py`; checked with napari 0.10): from the unpacked zip it
  opens that folder; a copy kept beside the zips (in Downloads, say) opens the newest recording zip next to it; or
- run the zip:

```bash
python insiliscope_live_128x128x10.zip
```

Either way napari shows every part of the recording in world coordinates (µm, z above the coverslip), so they line up:

- **movie**: the frames where the camera saw them (the stage position, the pixel size). In 2D it stands at every z (a
  camera image is a projection); in 3D it is a plane at the focal height.
- **density** stacks (`gt_volume_*.tif`), a layer per target, coloured.
- **localizations** of the lowest-threshold table, each at its own frame, coloured by target, with their photons as a
  feature; "all frames" (hidden) holds them all at once, for 3D.
- **beads** (`fiducials.csv`), per frame.

The axes are named frame, z, y, x (µm); the status bar shows the pointer's world position. From a terminal it takes paths too:
`python open_in_napari.py <zip, folder or voxelize .tif> ...` opens several side by side. A voxelize `.tif` opens a layer per channel, read a plane at a time (with `zarr` and
`dask` installed), so a stack of many GB opens at once. The program is also
[published beside the viewer](https://kjamartens.github.io/inSiliScope/open_in_napari.py).

## Caveats

- **Orientation is not imaged yet.** The PSF is isotropic, so the dipole columns describe the label (Fixed or Random
  orientation in its settings) but did not shape the image. They are empty for a freely rotating dye, the default.
- **The PSF window.** "On the camera" counts the PSF over its kernel window (`psf-kernel-half-width-nm`) after the
  Quality's halo cut. A blink just outside the image can still put photons on it; it is listed (with an empty
  `bg_centre_photons`) when they reach the threshold.
- **Drift** is in the camera coordinates, not in the world ones. The drift per frame is in `frames.csv`.
