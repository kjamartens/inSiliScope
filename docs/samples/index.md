# Samples

The microscope images one **sample** at a time. A sample (in the code: a *specimen*) is a geometry: where its labelled
structures are, as a pure function of the seed and the position, over an endless coverslip. Everything after
that does not depend on the sample: the dyes and their photophysics, the light path, the PSF, the camera and the drift
([Physics](../physics/dyes-and-light-path.md)). A labelled structure of a sample is a **target**. Each target has its
own label (a dye in a mode, a labelled %, a DNA-PAINT imager concentration), and only the mounted sample's labels make
light.

| Sample | What it is | Targets | Pages |
|---|---|---|---|
| **Cell field** (`CellField`, the default) | Adherent cells packed on the coverslip: cell outline, cytoplasm height, nucleus, microtubules, nuclear pores | Microtubules (`mt-*`), nuclear pores (`npc-*`), imaged together; the nuclear lamina (`lamin-*`), unlabelled by default | [World model](cell-field/world-model.md), [Structures](cell-field/structures.md) |
| **Nanorulers** (`Nanorulers`) | DNA-origami rods lying on the coverslip, each with three spots of docking strands 80 nm apart; bent in the plane as a worm-like chain | Docking strands (`ruler-*`) | [DNA-origami nanorulers](nanorulers/index.md) |

On top of either sample, optional **fiducial beads** (gold, TetraSpeck, FluoSpheres, nanodiamonds) mark the drift: on
the coverslip, on the sample or in a gel layer, off by default ([Fiducial beads](fiducials.md); viewer: Fiducial beads,
Micro-Manager: `Fiducials`, CLI: `--fid-*`). Changing them is not a new sample: the illumination history stays.

The registry of samples and targets is `data/specimens.json`: each target's option prefix and core structure id, and
(in `data/dyes/library.json` `typicalLabels`) its usual dye and labelled % per label mode.

## Switching samples

| Where | How |
|---|---|
| Viewer | **Specimen** select (Sample section). Switching shows that sample's option groups and label, and zooms to a scale that suits it. |
| Micro-Manager | `SampleHolder` (a state device: `CellField`, `Nanorulers`), or the `Specimen` config group of the shipped configurations. Each sample has its own device for its geometry and label (`CellField`, `Nanorulers`). |
| CLI | `--specimen CellField` (default) or `--specimen Nanorulers` (or 0 / 1); the nanorulers' options are `--origami-*` and their label's `--ruler-*`. |
| webSMLM block | `CellField.buildWindow(w, h, { specimen: 'Nanorulers', params: { origamiDensity: 0.5, ... } })`. |

What a switch does:

- **It is a new sample.** In Micro-Manager the illumination history (where the sample was lit, so bleached or
  activated) and the sample's drift start afresh, the same as after a new seed. Switching back gives the first sample
  again, as it was before it was ever lit.
- **The focus stays.** The cell field's start focus is 0.5 µm above the coverslip; the nanorulers' dyes lie 13.5 nm
  above it, so set the `ZStage` (the viewer's Focus height) to about 0 - 0.02 µm for them. The CLI focuses on the
  docking strands by itself when `--z` is not given.
- **Each sample keeps its settings.** The cell field's and the nanorulers' properties and labels exist side by side;
  switching changes none of them. The camera's gain preset and the light path follow the mounted sample's labels (the
  light path: every labelled target's preset combined, `light-preset` auto).
- **BrightField** shows only what the sample's geometry gives it ([optics](../physics/optics.md#brightfield-imaging)): the
  cells' refractive index. DNA origami 13 nm thick gives no contrast at these NAs, so the nanorulers' BrightField
  frames hold the lamp and the camera noise only.

Adding a sample (a new geometry in the core, its registry entry, its Micro-Manager device): see
[Extending](../extending.md#recipe-a-new-sample).
