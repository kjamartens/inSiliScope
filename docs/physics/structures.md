# Structures

Each cell is generated in its own local frame (origin at the cell centre, units um, \(z\) = height above the coverslip)
and mapped to the world by the cell's packing rotation and position.

## Nucleus

A 3D ellipsoid per cell: long axis, short/long ratio and height (as a fraction of the long axis) are drawn from ranges
(defaults: long axis 8-12 um, ratio 0.6-1, height 0.3-0.6 of the long axis). Nucleus size is *correlated* with cell size by
reusing the same hash draw (same percentile of both ranges), and offset from the cell centroid by up to a fraction of the
cell radius. A lateral/vertical envelopment step guarantees the whole ellipsoid stays inside the cell with a margin.

## Cytoplasm

A height field over the cell footprint: dome over the nucleus (slope capped, `cytoDomeSlope`), a saturating rise from the
edge, `Hc(1 - e^{-s d_{edge}/Hc})` with \(H_c = 2 h_{mid}\), so the cell never has a linear pyramid flank. That raw
profile is a min/max of distance fields and has creases ("folds"), so the height is *relaxed*: on a 0.25 um grid it solves
\(h - \ell^2 \nabla^2 h = h_{raw}\) (\(\ell\) = `cytoRelaxUm`, default 1 um), a membrane under tension pulled toward the
profile, with \(h = 0\) on the exact outline and \(h \ge\) nucleus top + margin over the nucleus. Microtubules, dyes and
the brightfield volume all use this relaxed height, not the raw analytic one.

## Microtubules

Each microtubule is a 3D path, anchored at the nucleus surface:

1. **Start** on the nucleus ellipsoid (azimuth weighted by how much cytoplasm lies in each direction; polar angle uniform on
   the sphere), pushed outward by a random distance.
2. **End** near the cell edge with an end-direction jitter (up to 180 deg, so microtubules can cross over or under the nucleus).
3. **Path**: a correlated random walk in \(xy\) with a bounded turn radius, forced onto both endpoints with a
   Brownian-bridge drift correction. The persistence length is shared between the heading walk and \(z\), implemented so that
   the correlation length is fixed in real um regardless of step length (a discretised Ornstein-Uhlenbeck process in arc length;
   the heading kick scales as \(\sqrt{\ell_{step}/\ell_{corr}}\), the worm-like-chain relation).
4. **Height** is a *fraction of the local cytoplasm ceiling*, interpolated between start and end and converted using the
   ceiling at each point, so paths ride the actual slope; over/under-nucleus crossings apply a floor/ceiling on top; a moving
   average and a slope cap remove residual jitter.
5. Points are kept inside the cell volume; the walk is truncated at the first exit from the footprint (no edge hugging); a
   bounded minimum-separation pass separates microtubules in 3D.

Density is given per um\(^2\) of cell footprint (default 0.9/um\(^2\)).

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

**Labelling populations.** One draw per site, \(u\):

- \(u < f_{bleach}\): a **bleaching** dye (PALM/dSTORM-like, finite lifetime);
- \(f_{bleach} \le u < f_{bleach} + f_{persist}\): a **persistent** site (DNA-PAINT-like: never bleaches, constant supply of binding events);
- otherwise unlabelled.

The bleaching set never depends on the persistent fraction. Defaults: 0% bleaching, 70% persistent.

## Adding the missing structures

Lamins, mitochondria, NPCs, DNA and other cell types are not implemented yet; see [Extending](../extending.md) for how a new
per-cell generator keyed off the same `(seed, address, channel)` scheme plugs in.

Further details and the history of each fix: `spec/ALGORITHM.md`.
