# Structures

Each cell is generated in its own local frame (origin at the cell centre, units um, \(z\) = height above the coverslip)
and mapped to the world by the cell's packing rotation and position.

## Nucleus

A shaped ellipsoid per cell: long axis, short/long ratio and height (as a fraction of the long axis) are drawn from ranges
(defaults, all *estimates*: long axis 8-12 µm, ratio 0.6-1, height 0.2-0.3 of the long axis). Nucleus size is *correlated*
with cell size by reusing the same hash draw (same percentile of both ranges), and offset from the cell centroid by up to
a fraction of the cell radius.

Real nuclei are not ellipsoids: their outlines are smooth egg, bean or rounded-triangle shapes with radius deviations of
a few to ~10 %, and adherent nuclei are wider at the base (*estimates* from images; no measured shape distribution is
used). Each nucleus therefore gets, from its own hash stream (every default below an *estimate*):

- **lobes**: the footprint radius is \(1 + \sum_{k=2}^{8} (a_k \cos k\theta + b_k \sin k\theta)\) (soft-clamped), with a
  \(k^{-\gamma}\) spectrum (`nucSmooth`, default 2.5) normalised so the rms relative deviation is the cell's
  irregularity (`nucIrregMin`-`nucIrregMax`, default 0.03-0.2);
- a **kidney bend** (`nucBendMin`-`nucBendMax`, 0-0.3): an invertible shear of the outline;
- an **uneven thickness** (`nucThickIrreg`, 0.1 rms at the edge);
- a **wider base** (`nucAsym`, 0.5: sections below the widest one are widened toward it; negative values widen the top)
  and a **lowered widest point** (`nucWidestMin`-`nucWidestMax`, 0.2-0.4 of the height above the bottom).

All shape terms at 0 (and the widest point at 0.5) give the plain ellipsoid. The nucleus sits on a thin basal layer of
cytoplasm: its bottom is `nucBaseMin`-`nucBaseMax` (0.4-0.9 µm, *estimate*) above the coverslip, and the dome top follows
the nucleus top plus the margin (`nucMargin`, 0.5 µm, *estimate*). A lateral envelopment step keeps the whole footprint
inside the cell outline with that margin.

In Micro-Manager these are the `CellField.Nuc*` properties (`NucBaseMinUm`/`MaxUm`, `NucIrregMin`/`Max`,
`NucBendMin`/`Max`, `NucSmooth`, `NucThickIrreg`, `NucAsym`, `NucWidestMin`/`Max`); in the cli and viewer, `p.<name>`.

Shaped nuclei next to the plain ellipsoids the same cells would have with every shape term at 0:

<!-- fig:struct-nucleus -->

## Cytoplasm

A height field over the cell footprint: dome over the nucleus (slope capped, `cytoDomeSlope`), a saturating rise from the
edge, `Hc(1 - e^{-s d_{edge}/Hc})` with \(H_c = 2 h_{mid}\), so the cell never has a linear pyramid flank. Defaults
(*estimates*; the viewer's "Rounded" look since 2026-10-03): rim height 0.2-0.5 µm, mid height 2-3.5 µm, slope caps 2
(cytoplasm) and 4 (dome); the viewer's **Cell look** presets set other profiles. That raw
profile is a min/max of distance fields and has creases ("folds"), so the height is *relaxed*: on a 0.25 µm grid it solves
\(h - \ell^2 \nabla^2 h = h_{raw}\) (\(\ell\) = `cytoRelaxUm`, default 1 µm, an *estimate*), a membrane under tension
pulled toward the profile, with \(h = 0\) on the exact outline and \(h \ge\) nucleus top + margin over the nucleus.
Microtubules, dyes and the brightfield volume all use this relaxed height, not the raw analytic one.

The raw and the relaxed height of one cell:

<!-- fig:struct-cytoplasm -->

## Microtubules

Each microtubule is a 3D path from near the nucleus to near the cell edge:

1. **Start**: a point of the cytoplasm volume with density \(\propto e^{-d/\lambda}\), \(d\) the distance to the nucleus
   surface and \(\lambda\) = `mtStartDecayPct` (1.6 %, *estimate*) of the cell's equivalent diameter (~0.5 µm).
2. **End**: twelve candidate points of the footprint outside the nucleus, density \(\propto e^{-d/\lambda}\) with \(d\) the
   distance to the outline (`mtEndDecayPct`, 20 %, ~6 µm, *estimate*); one is picked with weight \(e^{\kappa(\cos a - 1)}\),
   \(a\) the angle between start-to-end and the outward direction at the start (`mtDirKappa`, 1.5, *estimate*; 0 = any
   direction, so paths also cross over or under the nucleus).
   Micro-Manager: `CellField.MicrotubuleStartDecayPct`, `...EndDecayPct`, `...DirKappa`.
3. **Path**: a correlated random walk in \(xy\) with a bounded turn radius, forced onto both endpoints with a
   Brownian-bridge drift correction. The persistence length is shared between the heading walk and \(z\), implemented so that
   the correlation length is fixed in real um regardless of step length (a discretised Ornstein-Uhlenbeck process in arc length;
   the heading kick scales as \(\sqrt{\ell_{step}/\ell_{corr}}\), the worm-like-chain relation). The correlation length
   (`mtSmoothLen`, default 1.5 µm) is an *estimate* for microtubules in cells, far below the thermal persistence length of
   taxol-stabilized microtubules in vitro, 5.2 mm [[gittes1993](../../references.md#gittes1993)].
4. **Height** is a *fraction of the local cytoplasm ceiling*, interpolated between start and end and converted using the
   ceiling at each point, so paths ride the actual slope, then box-smoothed. Where a path crosses the nucleus it rides over
   (or, starting low, under) it with a clearance, lifted or lowered by a smooth ramp rather than point by point (a generic
   obstacle interface: the nucleus is the first obstacle). A slope cap removes residual jitter.
5. Points are kept inside the cell volume; the walk is truncated at the first exit from the footprint (no edge hugging); a
   bounded minimum-separation pass separates microtubules in 3D.

Density is given per µm\(^2\) of cell footprint (default 0.9/µm\(^2\), an *estimate*).

The microtubules of one cell from above and from the side, and with the direction preference switched off:

<!-- fig:struct-microtubules -->

## Fluorophore sites (dye lattice)

A microtubule is a 25 nm cylinder [[mikhaylova2015](../../references.md#mikhaylova2015), [weber1978](../../references.md#weber1978)]
carrying the 13_3 protofilament lattice: 13 protofilaments [[tilney1973](../../references.md#tilney1973)], protofilament \(k\)
at angle \(\phi_0 + 2\pi k/13\) and axial offset \((3 k \cdot 8/13) \bmod 8\) nm, a site every 8 nm dimer (about 1625
sites per µm). In the measured B-lattice the 4 nm monomers lie on a 3-start helix, so neighbouring protofilaments are
staggered by 12 nm/13 = 0.92 nm and the lattice has a seam [[mandelkow1986](../../references.md#mandelkow1986)]; the model's
1.85 nm step (3 dimers per turn) closes without a seam, a simplification that keeps the sites per µm.

| quantity | value |
|---|---|
| attachment radius | 12.5 nm from the axis (half the 25 nm diameter [[mikhaylova2015](../../references.md#mikhaylova2015)]) |
| binder (nanobody/antibody) stalk | 12 nm radially outward (*estimate*) |
| dye displacement from the binder tip | uniform in volume, 2-5 nm, uniform direction (linker; *estimate*) |

Stalk plus linker widen a labelled microtubule by up to 34 nm (2 x (12 + 5) nm), within the 20-40 nm of common labels
[[mikhaylova2015](../../references.md#mikhaylova2015)]; primary plus secondary antibodies take it from 25 to about 60 nm
[[weber1978](../../references.md#weber1978)], nanobodies add less [[mikhaylova2015](../../references.md#mikhaylova2015)].

Sites are **never materialised for a whole field**. A dye's identity is a hash of `(cell, microtubule, protofilament,
dimer)`; whether it is labelled, whether it ever activates and what it does are decided from hashes first, and 3D positions are
only computed for dyes that will emit. The unit of generation is a *block*: 1 µm of one microtubule.

The lattice as the core generates it, read back through the cli's dye-site output:

<!-- fig:struct-lattice -->

**Labelling.** Each structure carries one label (issue 16): a share of its sites (`density`, cli/viewer `mt-label-pct`
/ `npc-label-pct`, MM `CellField.Microtubules_LabelingPct` / `NuclearPores_LabelingPct`) carries a dye. One LABEL draw per site, \(u < \) density, picks them
(the same threshold as before, so a density equal to the old bleaching + persistent fractions gives the same dyes);
a second, nested FLUOR draw keeps the dye's fluorescent fraction (none at fraction 1). What the dyes do depends on the
label's mode (dSTORM, PALM, DNA-PAINT, WideField): [Photophysics](../../physics/photophysics.md). Each dye also has an orientation
(free, fixed or random, with a wobble cone; its own draws), not used by the renderer yet. Default: DNA-PAINT on 70% of
the sites (an *estimate*).

## Nuclear pore complexes

The cell field's second target (2026-10-12), labelled and imaged together with the microtubules. Each pore is the
Nup96 model of webSMLM [[thevathasan2019](../../references.md#thevathasan2019)]: two rings 50 nm apart along the pore
axis (the cytoplasmic one outside the nucleus, the nucleoplasmic one inside), eight corners per ring at a radius of
53.5 nm, four attachment points per corner on an arc 12 nm across, so 64 sites per pore.

- **Where.** Pores cover the whole nuclear envelope, uniformly by area (a fixed triangle mesh of the shaped nucleus
  picks the place, which is then put exactly on the membrane), each along the local outward normal with a random twist
  about it, so pores at the bottom of the nucleus are seen from below and those on its flanks from the side.
- **How many.** `npc-density` (MM `CellField.NuclearPoreDensityPerUm2`, Advanced) per µm² of envelope, default 8: the
  count is that times the envelope's area, rounded, so a lower density keeps a subset of the same pores. Measured
  interphase densities are 5-9 per µm² [[maeshima2006](../../references.md#maeshima2006),
  [dultz2010](../../references.md#dultz2010)]. A new pore closer than `npcMinDistNm` (MM
  `CellField.NuclearPoreMinDistNm`, Expert; default 180 nm, a pore being about 120 nm across) to one already placed
  is redrawn.
- **Label.** Default DNA-PAINT with a Cy3B imager at 561 nm on 60% of the sites: 2.5 nM (`npc-imager-nm`, MM
  `NuclearPores_ImagerNanoMol`; *estimate*, the microtubules' is 1 nM), binding as the microtubules' imager does
  (k<sub>on</sub> 10⁶ M⁻¹ s⁻¹, mean bound time 0.05 s, *estimates*; `npc-paint-kon`, `npc-paint-on-sec`, MM
  `NuclearPores_KonPerMolarPerSec`, `_BoundTimeSec`); the typical labels per mode follow
  the pore literature (dSTORM: Nup96-SNAP + BG-AF647, 58%; PALM mEos3.2; WideField mEGFP)
  [[thevathasan2019](../../references.md#thevathasan2019),
  [schlichthaerle2019](../../references.md#schlichthaerle2019)]. Set the pore label to None (`npc-dye None`) to image
  the microtubules alone, as before.
- **Brightfield.** Pores add no brightfield contrast (sub-resolution, far below the cell's phase).

<!-- fig:struct-npc -->

## Nuclear lamina

The cell field's third target (2026-10-27), **unlabelled by default**: it shows only once you give it a dye. For now
a random stand-in for the lamin network under the nuclear envelope: points, not filaments.

- **Where.** Points cover the whole nuclear envelope, uniformly by area and independently of each other (the pores'
  mesh picks the place, which is then put exactly on the membrane), but none within 20 nm of a pore's centre: a disc of
  `p.laminNpcExclNm` (MM `CellField.NuclearLaminaPoreExclusionNm`, Expert; default 40 nm, *estimate*) across around
  every pore, whether the pores are labelled or not.
- **How many.** `lamin-density` (MM `CellField.NuclearLaminaDensityPerUm2`, Advanced; 100 to 10 000 in the viewer and
  Micro-Manager) per µm² of envelope, default 2000 (*estimate*): the count is that times the envelope's area, rounded
  (some 250 000 on a nucleus of ~125 µm²). The viewer draws the points with View > Nuclear lamina.
- **Label.** One docking site per point, labelled through the same path as the other targets (`lamin-dye`,
  `lamin-mode`, `lamin-label-pct`, `lamin-imager-nm`, the linkage `lamin-binder*`; MM `NuclearLamina_*`). The linkage
  points inward, along the envelope's inward normal (the lamina lines the nucleoplasmic face; *estimate*); default
  anti-lamin primary + secondary antibodies (IgG2-PAINT). The typical labels per mode (DNA-PAINT Cy3B 70%, dSTORM AF647
  70%, PALM mEos3.2 58%, WideField mEGFP 100%) and the DNA-PAINT binding (2.5 nM imager, the pores' k<sub>on</sub> and
  bound time) are *estimates*. To turn it on: `lamin-dye Typical` on the cli, the Dye select of the Nuclear lamina
  label group in the viewer, `CellField.NuclearLamina_Label` in Micro-Manager.
- **Brightfield.** No brightfield contrast.

## Label linkage: where the dye sits

A dye does not sit on its structure: a probe holds it there. One model for every target: the **binder** (the probe:
primary + secondary antibodies, a nanobody, a SNAP or Halo tag, a fusion protein, a docking strand) points away from
the structure (radially from a microtubule, from a pore's corner outwards, from a lamina point inwards) by its length, with a flexibility (a random
offset in a ball); then a short chemical **linker** in a random direction; then the **dye**. Presets (cli/viewer
`<prefix>-binder`, MM `<Target>_Binder`, Advanced; the numbers in Expert) come from measured label shells
[[fruh2021](../../references.md#fruh2021), [nieves2019](../../references.md#nieves2019)]. Defaults: the microtubules
through primary + secondary antibodies (IgG2-PAINT: the dyes on a shell ~32 nm from the axis around the 12.5 nm tube, as measured for DNA-labelled secondaries), the
pores on a SNAP/Halo tag (a few nm). `Legacy` keeps the numbers from before the presets (12 nm + a 2-5 nm linker on a
microtubule).

<!-- fig:struct-linkage -->

## Adding the missing structures

A lamin network (beyond the random lamina points above), mitochondria, DNA and other cell types are not implemented
yet; see [Extending](../../extending.md) for how a new
per-cell generator keyed off the same `(seed, address, channel)` scheme plugs in.

Further details and the history of each fix: `spec/ALGORITHM.md`.
