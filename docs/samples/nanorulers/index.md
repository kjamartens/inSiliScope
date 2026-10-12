# DNA-origami nanorulers

DNA-origami nanorulers are reference samples for super-resolution microscopy: a stiff DNA rod with marks placed on it
to nanometre accuracy, so that a microscope's image can be checked against a known distance
[[scheckenbach2020](../../references.md#scheckenbach2020)]. inSiliScope's nanoruler sample is a field of such rods
lying flat on the coverslip, each carrying three spots of DNA-PAINT docking strands 80 nm apart. Mount it with the
viewer's **Specimen** select, Micro-Manager's `SampleHolder` (or the `Specimen` config group) or the CLI's
`--specimen Nanorulers` ([Samples](../index.md)). The dyes, the light path, the PSF and the camera are the
same as for the cell field ([Physics](../../physics/dyes-and-light-path.md)).

Each molecule goes through: the design at rest → which strands carry a dye (the label) → bending → landing (roll,
height) → placement (position, direction). Every step is a pure function of the seed and the molecule's address, so
any window of the field can be made alone, in any order, with the same result.

## The design

The 80 nm nanoruler (`Nanoruler80`, `data/origami/designs.json`):

- **Body:** a cylinder 200 nm long and 13 nm in diameter, the size of the typical nanoruler, a 12-helix bundle
  [[scheckenbach2020](../../references.md#scheckenbach2020)] (p. 110902-3).
- **Spots:** A, B and C at 20, 100 and 180 nm along it: 80 nm from A to B and from B to C, 160 nm from A to C.
- **Strands:** each spot is a ring of 5 docking-strand positions, 72° apart around the rod (radius 6.5 nm). This is a
  modelling choice, not a product's specification; no published strand count per spot was found. `StrandsPerSpot`
  (1-5) keeps positions 1..N of each ring, the same subset on every rod.

The schema is ready for 3D designs (a body shape, a deformation model, a landing model and spot lists). Only what
the 80 nm ruler needs is implemented: a cylinder, the in-plane bending below or none, and a uniform or fixed roll.
Other bodies (cuboids, meshes), 3D bending, stiffness patches, built-in curvature and explicit strand lists are
rejected as not implemented yet.

## The field

The coverslip is cut into 2 µm tiles. Each tile holds a Poisson number of candidate rods (mean = the density × 4
µm²; `DensityPerUm2`, default 0.3 µm⁻², *estimate*), each at a uniform position in the tile and a uniform direction.
A candidate is removed when a candidate of higher priority (a random number per candidate) lies within the minimum
spacing (`MinSpacingNm`, default 250 nm, *estimate*), looking across the neighbouring tiles too (Matérn type II
thinning). The rods left are therefore never closer than that, centre to centre, and the density after thinning is a
little below the set one. The draws for the field are separate from those for the bending, so changing the
persistence length never moves or turns a rod.

## Bending

A rod lying on the glass bends within the plane. Its centreline is a two-dimensional worm-like chain: a chain that
has equilibrated on the surface, as DNA can on a suitable one [[rivetti1996](../../references.md#rivetti1996)]. The
rod is cut into n = max(8, ⌈L / (D/2)⌉) = 31 segments of length Δs = L / n. Segment j turns the direction by a normal
angle step:

$$ \Delta\theta_j = z_j \sqrt{\Delta s / L_p}, \qquad z_j \sim \mathcal{N}(0, 1) $$

so the direction makes a random walk along the rod. Each segment is an exact circular arc of its own curvature, so the
contour length stays L. Cross-sections stay straight and normal to the centreline: the 13 nm body is drawn around it,
and a strand keeps its place along the rod and across it. The bent rod is then placed with the line between its ends
along its direction and the middle of that line at its position. For this chain:

- the end tangents differ by a normal angle of variance L / L<sub>p</sub>: 20.9° RMS at the default 1500 nm, 48.9° at
  273.1 nm;
- the tangent correlation over a contour s is ⟨cos Δθ(s)⟩ = e<sup>−s / 2L<sub>p</sub></sup> (the 3D chain's
  e<sup>−s / L<sub>p</sub></sup>, with half the freedom to turn);
- so the mean square distance between two points s apart is the 3D chain's formula with L<sub>p</sub> → 2L<sub>p</sub>:
  ⟨R²⟩ = 4 L<sub>p</sub> s [1 − (2L<sub>p</sub>/s)(1 − e<sup>−s / 2L<sub>p</sub></sup>)].

**The persistence length** (`PersistenceLengthNm`, Advanced, default **1500 nm**, an *estimate*). Measured bending
persistence lengths of DNA-origami helix bundles are 740 nm to 1 µm for four-helix, 1-3 µm for six-helix and 3-8.2 µm
for eight-helix bundles [[chhabra2020](../../references.md#chhabra2020)] (their summary of the measurements, p. 4 of
the arXiv version; simulations give somewhat higher values). A 12-helix rod is stiffer still, so 1500 nm leans to the
soft side. 273.1 nm (the value tuned by eye in the model's explorer, `tools/origami_reference/origami_bend.py`) bends
the rods visibly more. `Bending` Off makes every rod straight.

<!-- fig:ruler-shapes -->

How far bending moves the spots: the outer spots, 160 nm apart along the rod, are 158.6 nm apart (RMS, the formula
above) at 1500 nm and 152.6 nm at 273.1 nm; bending can only bring them closer.

<!-- fig:ruler-wlc -->

A bend sharper than the half-width of the rod (curvature × 6.5 nm ≥ 1) would fold a cross-section over itself; such
rods are counted (the geometry output's `fold`) but kept. None occur at the default persistence length.

## Landing and heights

Each rod lands rolled about its own axis: a uniform random roll per rod (`Roll` = `Uniform`), or one fixed angle for
all (`Fixed`, `RollDeg`). The roll turns each spot's ring of strands, so seen from above the strands of a spot lie at
different places across the rod. The rod's bottom is `AttachHeightNm` (7 nm, *estimate*: the surface coating) above
the glass.

The sample is two-dimensional (v1): every strand sits at the height of the rod's axis, 7 + 6.5 = **13.5 nm** above the
coverslip. `KeepHeights` (Expert) puts each strand at its ring height around the rod instead (from 7 to 20 nm), a first
step towards 3D designs, not validated yet.

## The label

The docking strands are the sample's one target (`docking`, options `ruler-*`, Micro-Manager `Nanorulers.Docking_*`).
They take a label like the cell field's microtubules: a dye of the library in a mode
([Dyes and light path](../../physics/dyes-and-light-path.md)).

- **Typical labels** (`data/dyes/library.json` `typicalLabels.docking`): an ATTO 655 imager in DNA-PAINT (the
  default), AF647 in dSTORM, mEos3.2 in PALM, ATTO 655 in WideField; the dye of a non-PAINT mode stands for a dye
  attached to the strand.
- **Labelled %** (`Docking_LabelingPct`, 80 %, *estimate*): the share of designed strands that were incorporated and
  carry a dye, drawn independently per strand.
- **Linker** (`LinkerNm`, 3 nm, *estimate*): each dye lies at a random point within this distance of its strand's
  anchor, on the outer side.
- **DNA-PAINT:** each strand binds imagers at k<sub>on</sub> × c (`Docking_ImagerNm`, 1 nM by default) and the free
  imager adds a uniform background ([photophysics](../../physics/photophysics.md)). With the library's ATTO 655 imager
  (k<sub>on</sub> 10⁶ M⁻¹ s⁻¹, 0.05 s bound, both *estimates*) a rod with 12 labelled strands binds an imager about
every 80 s per nM. At
  the defaults a movie is therefore sparse: few binding events per hundred frames over a field of view. Raise the
  imager concentration (with its background) and take thousands of frames, as in a real DNA-PAINT experiment.

<!-- fig:ruler-image -->

## Parameters

| Viewer | Micro-Manager | CLI | Default | Source |
|---|---|---|---|---|
| Specimen | `SampleHolder` | `--specimen` | CellField | |
| Design | `Nanorulers.Design` | `--origami-design` | Nanoruler80 | [[scheckenbach2020](../../references.md#scheckenbach2020)] (body) |
| Density | `Nanorulers.DensityPerUm2` | `--origami-density` | 0.3 µm⁻² | *estimate* |
| Strands per spot | `Nanorulers.StrandsPerSpot` | `--origami-strands-per-spot` | 5 | modelling choice |
| Bending | `Nanorulers.Bending` | `--origami-bend` | On | |
| Persistence length | `Nanorulers.PersistenceLengthNm` | `--origami-lp-nm` | 1500 nm | *estimate*, [[chhabra2020](../../references.md#chhabra2020)] (range) |
| Min spacing | `Nanorulers.MinSpacingNm` | `--origami-min-spacing-nm` | 250 nm | *estimate* |
| Height above glass | `Nanorulers.AttachHeightNm` | `--origami-attach-nm` | 7 nm | *estimate* |
| Linker | `Nanorulers.LinkerNm` | `--origami-linker-nm` | 3 nm | *estimate* |
| Roll, roll angle | `Nanorulers.Roll`, `RollDeg` | `--origami-roll`, `--origami-roll-deg` | Uniform, 0° | |
| 3D strand heights | `Nanorulers.KeepHeights` | `--origami-heights` | Off | |
| Docking strands label | `Nanorulers.Docking_Label`, `_Mode`, `_LabelingPct`, `_ImagerNm` | `--ruler-dye`, `--ruler-mode`, `--ruler-label-pct`, `--ruler-imager-nm` | Typical (ATTO 655 DNA-PAINT), 80 %, 1 nM | *estimates* |

The core parameters are the `origami*` names (`origamiLpNm`, ...; `p.<name>` on the CLI). The CLI's `--geometry-json`
lists each molecule in a window (position, direction, roll, end angle, fold, the bent outline and, with
`--geometry-detail 1`, every strand: spot, ring position, world position, straight and bent position in the
molecule's frame, labelled or not); the figures on this page are made from it.

## Limits

- Two-dimensional: the rods lie flat, the strands at one height; no 3D designs yet.
- Every rod is whole: no broken rods, missing spots or misfolded molecules beyond the labelled %.
- Uniform stiffness, no built-in curvature; rods may overlap where their bends cross (they are thinned by their
  centres only).
- No BrightField contrast (see [Samples](../index.md)).
- The viewer draws the rods in its map; its animation exports do not include them yet.
