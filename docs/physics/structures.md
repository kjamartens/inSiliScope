# Structures

Each cell is generated in its own local frame (origin at the cell centre, units um, \(z\) = height above the coverslip)
and mapped to the world by the cell's packing rotation and position.

## Nucleus

A shaped ellipsoid per cell: long axis, short/long ratio and height (as a fraction of the long axis) are drawn from ranges
(defaults: long axis 8-12 um, ratio 0.6-1, height 0.2-0.3 of the long axis). Nucleus size is *correlated* with cell size by
reusing the same hash draw (same percentile of both ranges), and offset from the cell centroid by up to a fraction of the
cell radius.

Real nuclei are not ellipsoids: their outlines are smooth egg, bean or rounded-triangle shapes with radius deviations of
a few to ~10 %, and adherent nuclei are wider at the base. Each nucleus therefore gets, from its own hash stream:

- **lobes**: the footprint radius is \(1 + \sum_{k=2}^{8} (a_k \cos k\theta + b_k \sin k\theta)\) (soft-clamped), with a
  \(k^{-\gamma}\) spectrum (`nucSmooth`, default 2.5) normalised so the rms relative deviation is the cell's
  irregularity (`nucIrregMin`-`nucIrregMax`, default 0.03-0.2);
- a **kidney bend** (`nucBendMin`-`nucBendMax`, 0-0.3): an invertible shear of the outline;
- an **uneven thickness** (`nucThickIrreg`, 0.1 rms at the edge);
- a **wider base** (`nucAsym`, 0.5: sections below the widest one are widened toward it; negative values widen the top)
  and a **lowered widest point** (`nucWidestMin`-`nucWidestMax`, 0.2-0.4 of the height above the bottom).

All shape terms at 0 (and the widest point at 0.5) give the plain ellipsoid. The nucleus sits on a thin basal layer of
cytoplasm: its bottom is `nucBaseMin`-`nucBaseMax` (0.4-0.9 um) above the coverslip, and the dome top follows the
nucleus top plus the margin (`nucMargin`, 0.5 um). A lateral envelopment step keeps the whole footprint inside the cell
outline with that margin.

In Micro-Manager these are the `CellField.Nuc*` properties (`NucBaseMinUm`/`MaxUm`, `NucIrregMin`/`Max`,
`NucBendMin`/`Max`, `NucSmooth`, `NucThickIrreg`, `NucAsym`, `NucWidestMin`/`Max`); in the cli and viewer, `p.<name>`.

Shaped nuclei next to the plain ellipsoids the same cells would have with every shape term at 0:

<!-- fig:struct-nucleus -->

## Cytoplasm

A height field over the cell footprint: dome over the nucleus (slope capped, `cytoDomeSlope`), a saturating rise from the
edge, `Hc(1 - e^{-s d_{edge}/Hc})` with \(H_c = 2 h_{mid}\), so the cell never has a linear pyramid flank. Defaults (the
viewer's "Rounded" look since 2026-10-03): rim height 0.2-0.5 um, mid height 2-3.5 um, slope caps 2 (cytoplasm) and 4
(dome); the viewer's **Cell look** presets set other profiles. That raw
profile is a min/max of distance fields and has creases ("folds"), so the height is *relaxed*: on a 0.25 um grid it solves
\(h - \ell^2 \nabla^2 h = h_{raw}\) (\(\ell\) = `cytoRelaxUm`, default 1 um), a membrane under tension pulled toward the
profile, with \(h = 0\) on the exact outline and \(h \ge\) nucleus top + margin over the nucleus. Microtubules, dyes and
the brightfield volume all use this relaxed height, not the raw analytic one.

The raw and the relaxed height of one cell:

<!-- fig:struct-cytoplasm -->

## Microtubules

Each microtubule is a 3D path from near the nucleus to near the cell edge:

1. **Start**: a point of the cytoplasm volume with density \(\propto e^{-d/\lambda}\), \(d\) the distance to the nucleus
   surface and \(\lambda\) = `mtStartDecayPct` (1.6 %) of the cell's equivalent diameter (~0.5 um).
2. **End**: twelve candidate points of the footprint outside the nucleus, density \(\propto e^{-d/\lambda}\) with \(d\) the
   distance to the outline (`mtEndDecayPct`, 20 %, ~6 um); one is picked with weight \(e^{\kappa(\cos a - 1)}\), \(a\) the
   angle between start-to-end and the outward direction at the start (`mtDirKappa`, 1.5; 0 = any direction, so paths
   also cross over or under the nucleus).
   Micro-Manager: `CellField.MicrotubuleStartDecayPct`, `...EndDecayPct`, `...DirKappa`.
3. **Path**: a correlated random walk in \(xy\) with a bounded turn radius, forced onto both endpoints with a
   Brownian-bridge drift correction. The persistence length is shared between the heading walk and \(z\), implemented so that
   the correlation length is fixed in real um regardless of step length (a discretised Ornstein-Uhlenbeck process in arc length;
   the heading kick scales as \(\sqrt{\ell_{step}/\ell_{corr}}\), the worm-like-chain relation).
4. **Height** is a *fraction of the local cytoplasm ceiling*, interpolated between start and end and converted using the
   ceiling at each point, so paths ride the actual slope, then box-smoothed. Where a path crosses the nucleus it rides over
   (or, starting low, under) it with a clearance, lifted or lowered by a smooth ramp rather than point by point (a generic
   obstacle interface: the nucleus is the first obstacle). A slope cap removes residual jitter.
5. Points are kept inside the cell volume; the walk is truncated at the first exit from the footprint (no edge hugging); a
   bounded minimum-separation pass separates microtubules in 3D.

Density is given per um\(^2\) of cell footprint (default 0.9/um\(^2\)).

The microtubules of one cell from above and from the side, and with the direction preference switched off:

<!-- fig:struct-microtubules -->

## Fluorophore sites (dye lattice)

A microtubule is a 25 nm cylinder carrying the 13_3 protofilament lattice: 13 protofilaments, protofilament \(k\) at angle
\(\phi_0 + 2\pi k/13\) and axial offset \((3 k \cdot 8/13) \bmod 8\) nm, a site every 8 nm dimer (about 1625 sites per um).

| quantity | value |
|---|---|
| attachment radius | 12.5 nm from the axis |
| binder (nanobody/antibody) stalk | 12 nm radially outward |
| dye displacement from the binder tip | uniform in volume, 2-5 nm, uniform direction (linker) |

Sites are **never materialised for a whole field**. A dye's identity is a hash of `(cell, microtubule, protofilament,
dimer)`; whether it is labelled, whether it ever activates and what it does are decided from hashes first, and 3D positions are
only computed for dyes that will emit. The unit of generation is a *block*: 1 um of one microtubule.

The lattice as the core generates it, read back through the cli's dye-site output:

<!-- fig:struct-lattice -->

**Labelling.** Each structure carries one label (issue 16): a share of its sites (`density`, cli/viewer `mt-label-pct`,
MM `CellField.Microtubules_LabelingPct`) carries a dye. One LABEL draw per site, \(u < \) density, picks them
(the same threshold as before, so a density equal to the old bleaching + persistent fractions gives the same dyes);
a second, nested FLUOR draw keeps the dye's fluorescent fraction (none at fraction 1). What the dyes do depends on the
label's mode (dSTORM, PALM, DNA-PAINT, WideField): [Photophysics](photophysics.md). Each dye also has an orientation
(free, fixed or random, with a wobble cone; its own draws), not used by the renderer yet. Default: DNA-PAINT on 70% of
the sites.

## Adding the missing structures

Lamins, mitochondria, NPCs, DNA and other cell types are not implemented yet; see [Extending](../extending.md) for how a new
per-cell generator keyed off the same `(seed, address, channel)` scheme plugs in.

Further details and the history of each fix: `spec/ALGORITHM.md`.
