# Port pending

Issue 12, "Nuclei are too spherical": the nucleus is no longer a plain ellipsoid. Real nuclei have smooth,
egg-, bean- or rounded-triangle-shaped outlines with radius deviations of a few to ~10 % (e.g. the control nuclei
in PMC5625896, Fig. 1), so the prototype now gives each cell's nucleus lobes, a kidney bend and an uneven
thickness, carried into 3D.

## What changed in the prototype

`web/prototype/index.html`, new section "Nucleus shape" (after `cellTailAt`):

- **Model.** The nucleus is the image of the unit ball under one smooth map. A ball point
  `(s cos t, s sin t, zeta)` maps to the footprint `(u, v) = s R(t) (cos t, sin t)`, then the bend
  `x = a u, y = b (v + bend (u^2 - 1/4))`, then `z = nucZ + rz H zeta`, in the nucleus frame (`nucRot`, `nucOff`;
  a, b, rz = nucLong/2, nucShort/2, nucHeight/2).
  - `R(t) = 1 + soft(sum_{k=2..8} Rc_k cos kt + Rs_k sin kt)` (soft clamp at 0.5);
  - `H(s, t) = 1 + soft(sum_{k=1..8} s^k (Hc_k cos kt + Hs_k sin kt))` (soft clamp at 0.6);
  - `soft(t) = t / sqrt(1 + (t/max)^2)`, as `cellTailAt`;
  - `cos kt`/`sin kt` and `(s e^{it})^k` come from complex-multiply recurrences (no trig per query).

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
  - `nucShaped`: any amplitude non-zero. When false, every caller keeps its old ellipsoid code, and the output is
    bit-identical to main (checked: 2.6M values over 5 cells, meshes and microtubules).
  - `nucHalfZ`: the vertical half-extent, the max of `H sqrt(1 - s^2)` over a 64 x 16 (t, s) sample grid;
    nucHeight/2 when unshaped.
  - `nucPoly`: the 256-point footprint polygon, cell-local.
  - `nucReach`: the farthest polygon point from the nucleus centre.
  - `nucCos`/`nucSin`.
  - `nucShapeSig`: a cache key.
- **Users of the nucleus:**
  - `envelopNucleus`: the vertical step uses `nucHalfZ`; the lateral step walks `nucPoly`.
  - `nucleusSignedDistLocal`: nearest point on `nucPoly` (squared distances, one sqrt), with the sign from `s < 1`.
  - `buildCytoHeightGrid`: `nucReach`, and the obstacle `nucZ + nucleusColumnLocal + margin`.
  - `nucleusColumnLocal(c, lx, ly)`: the chord half-height `rz H sqrt(1 - s^2)`, or -1 outside.
  - `nucleusRingsLocal(c, slices, pts)`: drawing rings (ball sections).
  - `cytoGeometrySignature`: gains the 6 new params.
- `microtubules.js`:
  - `mtNucleusRadiusAt`: bisection along the ray to s = 1, `MT_BISECT_ITERS` steps, bracket `[0, 1.01 nucReach]`.
  - `mtNucleusFootprintBlend`: uses `s`.
  - `mtClampIntoCytoplasm`: push-out in ball coordinates, scaling `(s, zeta)` and mapping back.
  - Start points: the same ball point `(sinPsi, phi0, cosPsi)` through `nucMapLocal`.
  - Over/under routing: uses `nucHalfZ`.
  - `mtCellShapeSig`: gains `nucShapeSig`.
- `web/prototype/scope/world.js` `opticalVolume`: the nucleus chord comes from `g.nucleusColumnLocal`, so
  BrightField sees the shaped nucleus.
- New params and defaults:

  | Param | Default | Meaning |
  |---|---|---|
  | `nucIrregMin`/`nucIrregMax` | 0.03 / 0.12 | rms relative radius deviation, per cell |
  | `nucBendMin`/`nucBendMax` | 0 / 0.5 | kidney bend, per cell |
  | `nucSmooth` | 2.5 | spectral slope |
  | `nucThickIrreg` | 0.1 | rms relative thickness variation at the edge |

  The slider extremes (irregularity 0.3, bend 1, height 0.4, smoothness 0) were checked: the nucleus is
  enveloped laterally and vertically, no microtubule point lies inside it, and the cytoplasm stays at least
  0.9 x margin above its top.

## Outside the prototype (JS only)

- `web/index.html` (the viewer):
  - Nucleus panel: Irregularity and Kidney bend pairs, plus Lobe smoothness and Height irreg. (advanced).
  - `params()` gains the 6 new keys; the current WASM ignores them (`nonCore`).
  - `drawNucleus(cell, asset)` draws `asset.nuc` (surface rings from the engine's `cell` reply) when present,
    else the old ellipsoid from the pack record.
- `web/lab/engine.js`: the `cell` reply gains `nuc` (Float64Array of rings x 48 points x xyz, cell-local; 9 ball
  sections with the poles dropped) and `nucPts`.
- `web/lab/field.js`, `lab.js`: the A/B top view draws the footprint polygon.
- `tests/parity/load_prototype.mjs`:
  - exports `nucleusColumnLocal` and `nucleusRingsLocal`;
  - an export missing from an older prototype (the A/B baseline) is now `undefined` instead of a ReferenceError.

## What the port must do beyond 1:1

- Core:
  - port `nucShapeInit`, `nucFootR`, `nucThickAt`, `nucMapLocal` and `nucBallLocal`, plus the callers above;
  - add the 6 params to the param table (`isc_params_set`);
  - the bend and the recurrences use only + - * / sqrt, and the spectrum uses `jsm::exp`/`jsm::log`.
- ABI: a nucleus-ring query for the viewer's `cell` job (`nuc`, `nucPts`), or the viewer draws nothing new.
  The ellipsoid fallback stays for the pack record.
- Optical volume (ABI 6): the chord from the shaped nucleus.
- Adapter: `SimType_CellFieldNucIrregMin/Max`, `NucBendMin/Max`, `NucSmooth`, `NucThickIrreg`, or whatever the
  CellField param bridge exposes. cli: the matching options.
- Re-freeze `spec/golden`, because the defaults change every cell's shape.
- Update `spec/ALGORITHM.md` and `docs/physics` (cell model), and add a gallery entry.
- `web/lab/engine_check.mjs` fails until then (lab engine vs main's WASM: shapes differ at the defaults).

Possible follow-up, not done: a flatter base for adherent nuclei (top/bottom asymmetry of H).
