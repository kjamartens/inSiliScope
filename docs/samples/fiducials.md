# Fiducial beads

Fiducial beads are bright, stable markers put on a sample to follow its drift: their positions over a movie are the
drift, which an SMLM analysis then subtracts. inSiliScope can put beads on either sample (the cell field and the
nanorulers), with a type, a density, a size and a height. They are **off by default** (density 0). Turn them on with
the viewer's **Fiducial beads** group (Sample section), Micro-Manager's `Fiducials` device, or the CLI's `--fid-*`
options (below). The beads belong to the sample: they drift with it, and the CLI's `<movie>.drift.csv` is their
ground truth.

## Bead types

Spectra and brightness follow Balinovic, Albrecht and Endesfelder 2019, who compared these markers for SMLM
[[balinovic2019](../references.md#balinovic2019)]. Where the paper has no spectrum, the spectra come from FPbase
[[lambert2019](../references.md#lambert2019)] and the nominal maxima from the manufacturer
[[thermofisher_fluospheres](../references.md#thermofisher_fluospheres)]. The data are in `data/fiducials/`
([README](https://github.com/kjamartens/inSiliScope/tree/main/data/fiducials)).

| Type (`Type`, `--fid-type`) | Size | Colours (ex/em, nm) | Bleaches | Notes |
|---|---|---|---|---|
| `Gold` (the default type) | 100 nm | photoluminescence ~560/575 | no | also scatters every laser line (Mie); the filters' blocking floor decides how much reaches the camera |
| `TetraSpeck` | 100 nm | 360/430, 505/515, 560/580, 660/680 | yes, each dye on its own | one bead in four channels |
| `FluoSpheresOrange` | 200 nm | 540/560 | yes | a marker that also shows in a 488 nm channel |
| `FluoSpheresRed` | 200 nm | 580/605 | yes | |
| `FluoSpheresDarkRed` | 200 nm | 660/680 | yes | very bright under 640 nm: can saturate a camera at SMLM powers |
| `FluoSpheresInfrared` | 100 nm | 715/755 | yes | red-shifted, for imaging next to a 640 nm channel |
| `Nanodiamond` | 100 nm | NV centres, ~640/680 | no | |

<!-- fig:fid-spectra -->

The brightness of each dye is an **equivalent dye count** (dyes with ε = 10⁵ M⁻¹ cm⁻¹ and QY 0.5) at the type's
product size, scaling with the bead's volume (d³). For gold, TetraSpeck and the nanodiamonds it was fitted to the
paper's counts: each bead's brightest pixel against single dyes (mEos3.2, JF646) in the same images. The FluoSpheres
are set to **1000 equivalent dyes** (an *estimate*): the paper's fits (41 000 for the dark red, 4 × 10⁶ for the
infrared, whose far tails the paper's curves underestimate) saturate a camera under the bead's own laser. The fits stay
in `data/fiducials/types.json` as `emitters`; `emittersSet` overrides them. The spectral losses stay in the light path,
not in the count. A dark-red bead under a 561 nm line behind a 595/50 filter, for example, keeps about 10⁻⁵ of its
light, which is why beads of hundreds to tens of thousands of equivalent dyes are usable next to single molecules. The
nanodiamonds' calibration also comes out unphysically high, for the same reason as the infrared beads'. Set
`DyeCount` (Expert, `--fid-dyes`) to override every component's count.

## Placement, size and height

- **Positions** are random, with no minimum distance: a Poisson field of `DensityPerUm2` beads per µm² of coverslip
  (`--fid-density`), so doublets and clusters happen as on a real coverslip. The positions are a pure function of the
  seed and the place, like every other part of the sample.
- **Size:** `DiameterNm` (Advanced, `--fid-diameter-nm`; −1 = the type's product size) with a bead-to-bead spread
  `DiameterCvPct` (12 %, an *estimate* within the 10-15 % typical of these products).
- **Height** (`Height`, `--fid-height`):
    - `Coverslip`: the bead's centre one radius above the glass, also under the cells (the beads go down first).
    - `OnTop` (default): on the sample's highest surface there (the cytoplasm or nucleus of a cell; the bare glass
      between cells), centre one radius above it.
    - `Layer`: anywhere in a gel layer (e.g. agarose) of `LayerUm` (10 µm) between the coverslip and the layer's
      top, outside the cells.

<!-- fig:fid-heights -->

## The image of a bead

A bead is not a point: a 200 nm sphere holds dyes over its whole volume. Its image is the PSF averaged over the solid
sphere (in z over 10 nm slabs, laterally over its projection), which matters within about a micrometre of the centre.
Outside that the bead's halo is the point PSF. Each bead is **one** emitter per colour, never 10⁶ dyes. Beads do not
blink.

A bead is far brighter than a dye, and above or below the focus its light spreads into a disc several micrometres
wide (about 2.4 µm of radius per µm of defocus at NA 1.4). So its halo is not cut at the PSF window the dyes use
(±3 µm in the viewer, ±7 µm in Micro-Manager): it comes from a second PSF window **twice the field of view** (at most
±30 µm), sampled once per camera pixel, and reaches as far as the bead gives at least 0.1 photon per pixel. A bright or
defocused bead lights the whole frame. Costs: the wide window takes ~0.5 s per colour once (256 px), and the beads take
a few per cent of a movie, about 15 % in a drifting one, where every bead is drawn again in every frame.

## Bleaching

The dyes in a bead bleach one by one, but there are so many that the bead's signal decays smoothly:
`exp(−t ln 2 / t½)`. `BleachHalfTimeSec` (Expert, `--fid-bleach-half-sec`, 28800 s = 8 h, an *estimate*) is the
half-time at `BleachRefKWcm2` (1 kW/cm², `--fid-bleach-ref-kwcm2`) at the dye's own excitation peak. The bleach rate
follows the excitation rate, so a bead bleaches more slowly at less light and off its peak: TetraSpeck's dark-red dye
under 561 nm fades about 10 times more slowly than at its peak at the same intensity. Gold and nanodiamonds do not bleach.

In Micro-Manager the beads fade with the sample's illumination history, like every dye: a bead that was lit stays
dimmer after a pause, a move away and back, or a change of a bead setting (which keeps the history). CLI and viewer
movies start at `--start-sec` under the movie's own light.

## Gold's scattered laser light

Gold nanoparticles scatter the excitation light elastically (Mie scattering; gold's optical constants from Johnson
and Christy [[johnson1972](../references.md#johnson1972)], the method of Bohren and Huffman
[[bohren1983](../references.md#bohren1983)]). Ideal filters would block it fully, so the emission path is given a
blocking floor: every laser line passes the dichroic and the emission filter at least at 10⁻ᴼᴰ (`BlockingOD`, Expert,
`--fid-blocking-od`, 8: a dichroic ~OD 2 times an emission filter ~OD 6, an *estimate*). The floor applies to the
scattered light only; the dyes' light path is unchanged.

## Settings

| Viewer (Sample > Fiducial beads) | Micro-Manager (`Fiducials`) | CLI | Default |
|---|---|---|---|
| Density | `DensityPerUm2` (Basic) | `--fid-density` | 0 (off) |
| Type | `Type` (Basic) | `--fid-type` | `Gold` |
| Diameter, Diameter spread | `DiameterNm`, `DiameterCvPct` (Advanced) | `--fid-diameter-nm`, `--fid-diameter-cv-pct` | the type's size, 12 % |
| Height, Layer thickness | `Height`, `LayerUm` (Advanced) | `--fid-height`, `--fid-layer-um` | `OnTop`, 10 µm |
| Bleach half-time, Reference intensity | `BleachHalfTimeSec`, `BleachRefKWcm2` (Expert) | `--fid-bleach-half-sec`, `--fid-bleach-ref-kwcm2` | 28800 s, 1 kW/cm² |
| Equivalent dyes | `DyeCount` (Expert) | `--fid-dyes` | −1 (the type's) |
| Filter blocking | `BlockingOD` (Expert) | `--fid-blocking-od` | 8 |

The viewer draws the beads in the world view and in its x-z side view as discs of their diameter (the **Fiducial
beads** checkbox, View).

Not modelled: blinking of bead aggregates, single-dye bleaching steps, and the beads' BrightField contrast.
