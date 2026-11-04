# Ground truth

After a **● Record**, **Save** can add the recording's settings and its ground truth (what made the frames) to the
movie, in one `.zip`. The command line writes the same files:

```bash
insiliscope_cli --settings settings.isc.json --out movie.tif --gt-out gt
```

The ground truth is computed on save, by rendering the recording again from its settings file; this starts as soon
as the Save box opens. With blinks, it starts with the Record when your last Save included the ground truth, so the
Save box has it at once. All photons are expected values, without noise.

## The files

| File | When | What |
|---|---|---|
| `movie.tif` | always | the frames, 16-bit (pixel size and frame interval in the header) |
| `settings.isc.json` | Settings file | reproduces the movie ([settings files](try-viewer.md)) |
| `gt/frames.csv` | Ground truth | one row per frame |
| `gt/localizations.csv` | blinking labels | blinks (and beads) with ≥ 300 photons in their core, per frame |
| `gt/fiducials.csv` | beads, no blinking label | the beads, per frame |
| `gt/dyes.csv` | Dye list (WideField) | one row per dye |
| `gt/gt_volume_<target>.tif` | continuous dyes | detected photons per voxel |
| `gt/voxelize.html` | Dye list | makes a stack at any voxel size from `dyes.csv` |
| `open_in_napari.py` | always | opens the recording in napari ([below](#open-it-in-napari)) |

### frames.csv

| Column | Meaning |
|---|---|
| `frame` | index from 0 |
| `t_start_s`, `exposure_s` | the sample's time at the frame's start; the exposure |
| `stage_x_um`, `stage_y_um` | the field of view's centre |
| `focus_um`, `focus_world_um` | the focus as set; the focal plane above the coverslip, drift included |
| `drift_x_nm`, `drift_y_nm`, `drift_z_nm` | the sample's drift |
| `pattern_angle`, `pattern_phase` | structured illumination only |

### localizations.csv

One row per blink per frame. A blink is listed when at least 300 of its photons (the Save box sets the threshold, the
cli `--gt-min`) land in the 500 × 500 nm square around it, its **core**. Counting the core rather than all photons keeps
out-of-focus blinks that spread their light too thin to see. The core is computed from the finely sampled PSF, so it
does not depend on the pixel size.

<!-- fig:gt-core -->

| Column | Meaning |
|---|---|
| `frame`, `t_s` | frame and its start time |
| `kind` | `emitter`, or `fiducial` for a bead |
| `id`, `target`, `dye`, `mode`, `state` | the dye (ids are stable across frames) and its label |
| `x [nm]`, `y [nm]` | position on the camera, from the image's top-left corner, drift included |
| `z [nm]` | height above the focal plane |
| `x/y/z_world_um` | position in the sample (z above the coverslip), without drift |
| `site_x/y/z_world_um` | the binding site; dye minus site is the linkage error |
| `on_fraction` | part of the frame the blink was on |
| `photons_detected` | photons collected |
| `intensity [photon]` | of those, the ones on the camera |
| `photons_emitted` | photons emitted |
| `offset [photon]` | other light per pixel under the blink (PSF-weighted): background, imager, continuous dyes; not other blinks (below) |
| `bg_centre_photons` | that light in the centre pixel |
| `core_photons` | photons in the 500 nm core (the threshold's number) |
| `wavelength_nm`, `excitation` | emission wavelength; excitation factor at the dye |
| `regime` | `splat`, `binned` (high density) or `bead` |
| `bead_diameter_nm` | beads only |
| `orient_polar_deg`, `orient_azimuth_deg` | the dye's dipole, if its label fixes one (not yet used by the PSF) |

Blinks that Quality left out of the image (under 100 photons at Fast, 50 at Realistic) are not listed either.

!!! note "The offset leaves out other blinks"
    `offset` and `bg_centre_photons` are the frame's light before any blink is drawn: background, imager and the
    continuous dyes. The other blinks of the same frame are not in them, not even an out-of-focus blink whose
    spread-out light lies under this one. So where blinks are dense, the movie has more light under a blink than
    `offset` says.

### dyes.csv and the density stacks

Continuous dyes (WideField, dSTORM's initial ON state, PALM's pre-state) glow throughout instead of blinking, so they
get no localization table.

- `dyes.csv` (WideField only, optional: 100-600 MB at 128 px): every dye's position, binding site, on/off times
  (`t_on_s`, `t_off_s`), current rate and photons over the recording.
- `gt_volume_<target>.tif`: the detected photons per voxel, 8-bit (photons = value × `insiliscope.scale`, Fiji's
  calibration). Voxels are 10 nm, or coarser to stay under 1 GB in all; the README says when. All targets share one
  grid, so the stacks overlay. Fiji reads the voxel size and world origin; open with **File › Import › TIFF Virtual
  Stack** to see each slice's z.

### voxelize.html

A stack from a table at any voxel size: ☰ › Tools › **Voxelize ground truth**
([online](https://kjamartens.github.io/inSiliScope/voxelize.html)), or `gt/voxelize.html` in a zip with the dye
list (works offline). Drop the zip or a table on it, then choose the positions (dyes or binding sites), the value
(detected or emitted photons, or a count), the targets (a channel each or one stack), voxel size, bit depth and file
type. It uses the same code as the density stacks.

## Open it in napari

Install `pip install "napari[all]" tifffile`, then **drop `open_in_napari.py` on napari's window**: it opens the
folder it lies in, or the newest recording zip beside it. Or:

```bash
python open_in_napari.py insiliscope_live_128x128x10.zip
```

Everything is placed in world µm:

- **movie**: the frames at the stage position, auto-contrasted.
- **gt_volume_…**: the density stacks; the z slider steps through them (the height is shown in the corner).
- **localizations …**: a layer per label at its frame; "…, all frames" (hidden) shows them all at once in 3D. Size
  (nm), symbol and colour apply to the whole layer when no point is selected.
- **beads**: `fiducials.csv`, per frame.

A recording without density stacks has no z axis. A voxelize `.tif` opens too, a layer per channel.
