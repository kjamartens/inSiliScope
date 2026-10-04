# Port pending

Issue 12, "Nuclei are too spherical": the nucleus is no longer a plain ellipsoid. Real nuclei have smooth,
egg-, bean- or rounded-triangle-shaped outlines with radius deviations of a few to ~10 % (e.g. the control nuclei
in PMC5625896, Fig. 1), and adherent nuclei are wider at the base. The prototype now gives each cell's nucleus
lobes, a kidney bend, an uneven thickness and a top/bottom asymmetry, carried into 3D.

## What changed in the prototype

`web/prototype/index.html`, new section "Nucleus shape" (after `cellTailAt`):

- **Model.** Shape coordinates: footprint radius `s` (1 = the outline), direction `t`, height `zeta` in [-1, 1].
  A point maps to the footprint `(u, v) = s R(t) (cos t, sin t)`, then the bend
  `x = a u, y = b (v + bend (u^2 - 1/4))`, then `z = nucZ + rz H zeta`, in the nucleus frame (`nucRot`, `nucOff`;
  a, b, rz = nucLong/2, nucShort/2, nucHeight/2).
  - `R(t) = 1 + soft(sum_{k=2..8} Rc_k cos kt + Rs_k sin kt)` (soft clamp at 0.5);
  - `H(s, t) = 1 + soft(sum_{k=1..8} s^k (Hc_k cos kt + Hs_k sin kt))` (soft clamp at 0.6);
  - `soft(t) = t / sqrt(1 + (t/max)^2)`, as `cellTailAt`;
  - `cos kt`/`sin kt` and `(s e^{it})^k` come from complex-multiply recurrences (no trig per query).
  - Inside: `|zeta| < 1` and `s < W(zeta)`. The section width `W(zeta) = q + (1 - q) f`, `q = sqrt(1 - zeta^2)`
    (`nucSectionW`), is the ellipsoid's q widened toward the mid-section by the fraction f of the gap.
    `f = nucAsym` on the bottom half when `nucAsym > 0`, `-nucAsym` on the top half when `< 0`, else 0;
    `nucAsym` is clamped to +-0.9 (`NUC_ASYM_MAX`). No section is wider than the mid-section.
  - A column at `s < 1` is one interval. On a side with widening f, its `|zeta|` extent is 1 if `s <= f`, else
    `sqrt(1 - qs^2)` with `qs = (s - f) / (1 - f)` (`nucColumnExt`).

  Inverse (`nucBallLocal`): rotate into the nucleus frame, `u = x/a`, `v = y/b - bend (u^2 - 1/4)`,
  `rho = sqrt(u^2 + v^2)`, `(C, S) = (u, v)/rho` (or (1, 0) at rho <= 1e-12), `s = rho / R`.
- **Draws** (`nucShapeInit`, called in `rawCandidate` right after `nucZ`): one `hashStream(seed, cx, cy, 52)`
  (`NUC_SHAPE_STREAM`; the TAIL streams are 50/51). The order is:
  1. irregularity percentile;
  2. bend percentile;
  3. for k = 2..8: Rc_k, Rs_k;
  4. for k = 1..8: Hc_k, Hs_k.

  Each coefficient is `amp * w_k * (2u - 1) * sqrt(3)`. The spectrum is `w_k = exp(-nucSmooth * log k)`,
  normalised to `sum w_k^2 = 1` (separately over k = 2..8 for R and k = 1..8 for H). `amp` is
  `lerp(nucIrregMin, nucIrregMax, u)` for R and `nucThickIrreg` for H; `nucBend = lerp(nucBendMin, nucBendMax, u)`.
- **Derived per-cell fields:**
  - `nucShaped`: any amplitude non-zero, or `nucAsym != 0`. When false, every caller keeps its old ellipsoid code,
    and the output is bit-identical to main (checked: 2.6M values over 5 cells, meshes and microtubules).
  - `nucFBot`/`nucFTop`: the widening f of each half.
  - `nucUp`/`nucDown`: the vertical extents above/below `nucZ`, the max of `H x column extent` (times rz) over a
    64 x 16 (t, s) sample grid; nucHeight/2 each when unshaped.
  - `nucPoly`: the 256-point footprint polygon, cell-local.
  - `nucReach`: the farthest polygon point from the nucleus centre.
  - `nucCos`/`nucSin`.
  - `nucShapeSig`: a cache key.
- **Users of the nucleus:**
  - `envelopNucleus`: the vertical step keeps `[nucZ - nucDown, nucZ + nucUp]` inside the margin; the lateral step
    walks `nucPoly`. The mid-section is the widest, so the asymmetry leaves the footprint unchanged.
  - `nucleusSignedDistLocal`: nearest point on `nucPoly` (squared distances, one sqrt), with the sign from `s < 1`.
  - `buildCytoHeightGrid`: `nucReach`, and the obstacle `nucZ + top + margin`. Shaped: `top` is the max of the
    column top over 3 x 3 samples at +-g/2 around the node, so the bilinear surface clears a steep rim (a wide
    top). Unshaped: the node's own column, as before.
  - `nucleusColumnLocal(c, lx, ly)`: `[extent below nucZ, extent above]`, each `rz H x nucColumnExt`, or null
    outside.
  - `nucPushOutLocal(c, pt, margin)`: pushes a point out radially from the centre in (s, zeta): the boundary scale
    by bisection (24 steps, from 1 to `1 / max(s, |zeta|)`), divided by the margin.
  - `nucleusRingsLocal(c, slices, pts)`: drawing rings, the horizontal sections at `s = W(zeta)` (those under
    0.05 dropped).
  - `cytoGeometrySignature`: gains the 7 new params.
- `microtubules.js`:
  - `mtNucleusRadiusAt`: bisection along the ray to s = 1, `MT_BISECT_ITERS` steps, bracket `[0, 1.01 nucReach]`.
  - `mtNucleusFootprintBlend`: uses `s`.
  - `mtClampIntoCytoplasm`: `nucPushOutLocal`.
  - Start points: the sphere point `(sinPsi, phi0, cosPsi)`, its radius widened like the section there
    (`sinPsi + (1 - sinPsi) f`), through `nucMapLocal`.
  - Over/under routing: uses `nucUp`/`nucDown`.
  - `mtCellShapeSig`: gains `nucShapeSig`.
- `web/prototype/scope/world.js` `opticalVolume`: the nucleus chord `[nucZ - below, nucZ + above]` comes from
  `g.nucleusColumnLocal`, so BrightField sees the shaped nucleus.
- New params and defaults:

  | Param | Default | Meaning |
  |---|---|---|
  | `nucIrregMin`/`nucIrregMax` | 0.03 / 0.2 | rms relative radius deviation, per cell |
  | `nucBendMin`/`nucBendMax` | 0 / 0.3 | kidney bend, per cell |
  | `nucSmooth` | 2.5 | spectral slope |
  | `nucThickIrreg` | 0.1 | rms relative thickness variation at the edge |
  | `nucAsym` | 0.8 | top/bottom asymmetry, -0.9..0.9: > 0 a wider base, < 0 a wider top |

  The slider extremes (irregularity 0.3, bend 1, height 0.4, smoothness 0, asymmetry +-0.9) were checked: the
  nucleus is enveloped laterally and vertically, no microtubule point lies inside it, and the cytoplasm stays
  at least 0.9 x margin above its top (measured minimum gap 0.59 um at margin 0.6: grid rounding).

## Outside the prototype (JS only)

- `web/index.html` (the viewer):
  - Nucleus panel: Irregularity and Kidney bend pairs and Top/bottom asym., plus Lobe smoothness and Height
    irreg. (advanced).
  - `params()` gains the 7 new keys; the current WASM ignores them (`nonCore`).
  - `drawNucleus(cell, asset)` draws `asset.nuc` (surface rings from the engine's `cell` reply) when present,
    else the old ellipsoid from the pack record.
- `web/lab/engine.js`: the `cell` reply gains `nuc` (Float64Array of rings x 48 points x xyz, cell-local; the
  `nucleusRingsLocal(c, 9, 48)` sections) and `nucPts`.
- `web/lab/field.js`, `lab.js`: the A/B top view draws the footprint polygon.
- `tests/parity/load_prototype.mjs`:
  - exports `nucleusColumnLocal` and `nucleusRingsLocal`;
  - an export missing from an older prototype (the A/B baseline) is now `undefined` instead of a ReferenceError.
- Lab tooling (`web/lab/serve.mjs`, `lab_html.js`, `lab.js`, README; nothing to port):
  - lab.html's About shows the served branch and commit;
  - starting the lab again stops the running one and takes its port, and open pages reload onto it.

## What the port must do beyond 1:1

- Core:
  - port `nucShapeInit`, `nucFootR`, `nucThickAt`, `nucSectionW`, `nucColumnExt`, `nucMapLocal`, `nucBallLocal`
    and `nucPushOutLocal`, plus the callers above;
  - add the 7 params to the param table (`isc_params_set`);
  - the bend and the recurrences use only + - * / sqrt, and the spectrum uses `jsm::exp`/`jsm::log`.
- ABI: a nucleus-ring query for the viewer's `cell` job (`nuc`, `nucPts`), or the viewer draws nothing new.
  The ellipsoid fallback stays for the pack record.
- Optical volume (ABI 6): the asymmetric chord from the shaped nucleus.
- Adapter: `SimType_CellFieldNucIrregMin/Max`, `NucBendMin/Max`, `NucSmooth`, `NucThickIrreg`, `NucAsym`, or
  whatever the CellField param bridge exposes. cli: the matching options.
- Re-freeze `spec/golden`, because the defaults change every cell's shape.
- Update `spec/ALGORITHM.md` and `docs/physics` (cell model), and add a gallery entry.
- `web/lab/engine_check.mjs` fails until then (lab engine vs main's WASM: shapes differ at the defaults).
