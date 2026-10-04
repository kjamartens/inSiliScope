# Port pending

Issue 12, "Nuclei are too spherical": the nucleus is no longer a plain ellipsoid. Real nuclei have smooth,
egg-, bean- or rounded-triangle-shaped outlines with radius deviations of a few to ~10 % (e.g. the control nuclei
in PMC5625896, Fig. 1), and adherent nuclei are wider at the base. The prototype now gives each cell's nucleus
lobes, a kidney bend, an uneven thickness, a top/bottom asymmetry and a lowered widest point, carried into 3D.

## What changed in the prototype

`web/prototype/index.html`, new section "Nucleus shape" (after `cellTailAt`):

- **Model.** Shape coordinates: footprint radius `s` (1 = the outline), direction `t`, height `zeta` in [-1, 1].
  A point maps to the footprint `(u, v) = s R(t) (cos t, sin t)`, then the bend
  `x = a u, y = b (v + bend (u^2 - 1/4))`, then `z = nucZ + rz k H zeta`, in the nucleus frame (`nucRot`, `nucOff`;
  a, b, rz = nucLong/2, nucShort/2, nucHeight/2).
  - `R(t) = 1 + soft(sum_{k=2..8} Rc_k cos kt + Rs_k sin kt)` (soft clamp at 0.5);
  - `H(s, t) = 1 + soft(sum_{k=1..8} s^k (Hc_k cos kt + Hs_k sin kt))` (soft clamp at 0.6);
  - `soft(t) = t / sqrt(1 + (t/max)^2)`, as `cellTailAt`;
  - `k = kDown = 2w` below the widest section (zeta < 0), `kUp = 2(1 - w)` above it: `w` in [0, 1] is the widest
    section's height as a fraction of the nucleus height, so the height stays `rz (kDown + kUp) = nucHeight`
    (times H); `nucZ` is the widest section's height;
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
  3. widest-point percentile: `w = clamp01(lerp(nucWidestMin, nucWidestMax, u))`;
  4. for k = 2..8: Rc_k, Rs_k;
  5. for k = 1..8: Hc_k, Hs_k.

  Each coefficient is `amp * w_k * (2u - 1) * sqrt(3)`. The spectrum is `w_k = exp(-nucSmooth * log k)`,
  normalised to `sum w_k^2 = 1` (separately over k = 2..8 for R and k = 1..8 for H). `amp` is
  `lerp(nucIrregMin, nucIrregMax, u)` for R and `nucThickIrreg` for H; `nucBend = lerp(nucBendMin, nucBendMax, u)`.
- **Derived per-cell fields:**
  - `nucShaped`: any amplitude non-zero, `nucAsym != 0`, or `w != 0.5`. When false, every caller keeps its old
    ellipsoid code. That was bit-identical to main (2.6M values over 5 cells) until the vertical placement below
    changed; now no setting reproduces main.
  - `nucBase`: the gap between the coverslip and the nucleus bottom, `lerp(nucBaseMin, nucBaseMax, u)` on the old
    `NUC_ZFRAC` channel (36), which no longer draws a fraction of the cell height.
  - `nucFBot`/`nucFTop`: the widening f of each half.
  - `nucKUp`/`nucKDown`: the vertical scales above/below the widest section (see Model).
  - `nucUp`/`nucDown`: the vertical extents above/below `nucZ`: `rz kUp` (`rz kDown`) times the max of
    `H x column extent` over a 64 x 16 (t, s) sample grid (1 without thickness variation or asymmetry);
    nucHeight/2 each when unshaped.
  - `nucPoly`: the 256-point footprint polygon, cell-local.
  - `nucReach`: the farthest polygon point from the nucleus centre.
  - `nucCos`/`nucSin`.
  - `nucShapeSig`: a cache key.
- **Users of the nucleus:**
  - `envelopNucleus`, vertical step: `nucZ = nucBase + nucDown` (the nucleus bottom sits `nucBase` above the
    coverslip) and the dome top `c.height = nucBase + nucDown + nucUp + margin` (the later floor
    `max(height, cytoRimHeight, cytoMidHeight)` still applies). This replaces the provisional
    `nucZ = height x lerp(0.4, 0.6)`, which left the nucleus floating. The cell-height draw is gone with it:
    `cellHeightMin/Max` are removed (prototype input, viewer control and its Cell shape presets) and `CH.HEIGHT` (6)
    is retired, not reused.
  - `envelopNucleus`, lateral step: walks `nucPoly`. The mid-section is the widest, so the asymmetry leaves the
    footprint unchanged.
  - `nucleusSignedDistLocal`: nearest point on `nucPoly` (squared distances, one sqrt), with the sign from `s < 1`.
  - `buildCytoHeightGrid`: `nucReach`, and the obstacle `nucZ + top + margin`. Shaped: `top` is the max of the
    column top over 3 x 3 samples at +-g/2 around the node, so the bilinear surface clears a steep rim (a wide
    top). Unshaped: the node's own column, as before.
  - `nucleusColumnLocal(c, lx, ly)`: `[extent below nucZ, extent above]`, each `rz k H x nucColumnExt`, or null
    outside.
  - `nucPushOutLocal(c, pt, margin)`: `zeta = (z - nucZ) / (rz k H)` with k by the side of nucZ; pushes the point
    out radially from the centre in (s, zeta): the boundary scale
    by bisection (24 steps, from 1 to `1 / max(s, |zeta|)`), divided by the margin.
  - `nucleusRingsLocal(c, slices, pts)`: drawing rings, the horizontal sections at `s = W(zeta)`,
    `zeta = -cos(pi i / (slices - 1))` (even in polar angle), both poles included, so the rings reach the true
    top and bottom.
  - `cytoGeometrySignature`: gains the 11 new params.
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
  | `nucAsym` | 0.5 | top/bottom asymmetry, -0.9..0.9: > 0 a wider base, < 0 a wider top |
  | `nucWidestMin`/`nucWidestMax` | 0.2 / 0.4 | height of the widest section (fraction of the height), per cell |
  | `nucBaseMin`/`nucBaseMax` | 0.4 / 0.9 | gap below the nucleus (um, slider 0..2), per cell |

  Changed defaults of existing params: `nucHeightMin/Max` 0.2 / 0.3 (were 0.3 / 0.5), `nucMargin` 0.5 (was 0.6;
  it now applies to the sides and top only).

  The slider extremes (irregularity 0.3, bend 1, height 0.4, smoothness 0, asymmetry +-0.9, widest point 0 and
  1) were checked: the
  nucleus is enveloped laterally and vertically, no microtubule point lies inside it, and the cytoplasm stays
  at least 0.9 x margin above its top (measured minimum gap at the defaults 0.52 um, margin 0.5: grid rounding).

## Microtubules near the nucleus (`microtubules.js`)

Paths were jagged where they met the nucleus: z followed the fraction-of-ceiling profile, was pulled toward
"clear the nucleus" by a footprint blend (`mtNucleusFootprintBlend`, `MT_NUCLEUS_CLEAR_BLEND`, removed), then capped
per point by a nominal-step slope limiter (removed) and clamped per point.

- **z of a path** (`mtGenerateOne`): the fraction profile with its noise as before, box-smoothed (`mtBoxSmooth`,
  prefix sums, window `round(mtSmoothLen / stepLen)` halved, endpoints kept), then `mtObstacleBounds` and
  `mtObstacleEnvelope`.
- **Obstacles** (`mtObstacles(cell, p)`, generic so other structures, e.g. the ER, can be added later): objects with
  `column(x, y)` -> `[bottom, top]` absolute z or null, `clearance`, `goOver(startZ)`. Only the nucleus for now
  (`mtNucleusObstacle`): column `[nucZ - below, nucZ + above]` from `nucleusColumnLocal`, clearance
  `mtNucleusClearance`, over when `nucZ - nucDown - clearance <= 0` or the start z >= `nucZ`. In C++ an interface
  or a small tagged struct; the loop over obstacles is in list order.
- **Bounds** (`mtObstacleBounds(pts, ceil, obstacles)`): `goOver` once per obstacle from `pts[0].z`; per point
  `hi[i]` = ceiling x `MT_CONTAIN_MARGIN`, `fade = min(1, arc / MT_OBST_CLEAR_RAMP_UM)` (1 um, xy arc from the start),
  and for each obstacle with a column, `clr = clearance x fade`:
  - going over: `lo[i] = max(lo[i], top + clr)`;
  - going under: `hi[i] = min(hi[i], bottom - min(clr, bottom / 2))`.

  Then `lo[i] = min(lo[i], hi[i])` for every point (conflicting bounds: the upper one wins).
- **`mtObstacleEnvelope(pts, lo, hi, half)`**, `half = max(1, round(mtSmoothLen / 2 / stepLen))`: lift, then lower.
  For each: `need[i] = max(0, lo[i] - z[i])` (or `z[i] - hi[i]`), 0 at the endpoints; `ramp` = forward then
  backward running max of `need` minus `MT_OBST_RAMP_SLOPE` (1) x xy step length; box-smooth `ramp` (interior),
  `corr[i] = max(smoothed, need[i])` (endpoints: `ramp`); `z[i] += corr[i]` (or `-=`) for interior points only.
- **`mtBoxSmooth`** uses a symmetric window `h = min(half, i, n - 1 - i)`: a window cut on one side pulled the
  first points toward the inner values, a jump next to the fixed endpoint (the old z smoothing had this too).

### Start and end points, direction

The start was a point on the nucleus ellipsoid moved out by a fraction of the centre-to-edge distance, the end a
fraction in from the edge along a jittered azimuth from a cytoplasm-weighted direction table. Neither fits the
shaped nucleus. Now both are points of the cytoplasm, weighted by distance to a surface, and the direction comes from
choosing the end:

- **Decay lengths** (`mtDecayLen(pct, geom)`): `max(MT_MIN_DECAY_UM, pct / 100 x sizeUm)`, `sizeUm` = the cell's
  equivalent diameter `2 sqrt(area / pi)` (geometry cache, shoelace area of the 48-point outline). At the default
  cell sizes (mean equivalent diameter 30.4 um, p10-p90 26-35 um) the defaults 1.6 % and 20 % are ~0.5 and ~6 um.
- **Start** (`mtSampleStart`): density `~ exp(-d / lambda)`, `lambda = mtDecayLen(mtStartDecayPct)`, over the cytoplasm volume, `d` =
  `mtNucleusGap(cell, x, y, z)` (approximate distance to the nucleus surface, -1 inside; see its comment: in the
  nucleus shape coordinates, lateral gap `gl = (s - W(zeta)) rDir` and vertical gap `gv` to the column, combined
  `gl gv / hypot(gl, gv)`; `rDir` = |`nucMapLocal(c, 1, C, S, 0)` - centre|; the plain ellipsoid with f = 0, H = 1).
  Rejection: per try four draws `x, y, z, u` (always, in that order), uniform in the nucleus bounding box
  (`mtNucleusBox`: polygon or rotated ellipse) grown by `L = min(5 lambda, rMax)`, `z` in `[0, nucZ + nucUp + L)`;
  inside the nucleus: next try; `accept = u < exp(-d / lambda)`; a rejected candidate is still kept as the fallback
  when it is the closest valid one so far; valid = `mtInCytoplasm` (`z > 0`, inside the outline and under the
  ceiling, both x `MT_CONTAIN_MARGIN`). At most `MT_SAMPLE_TRIES` (128) tries, then the closest valid candidate,
  else the rim point at `nucLong / 2 / margin` along `nucRot`, z = `nucZ`. `lambda >= MT_MIN_DECAY_UM` (0.02).
- **End** (`mtSampleEnd`, `lambda = mtDecayLen(mtEndDecayPct)`): per try three draws `theta = 2 pi u1`, `d = -lambda log(1 - u2)`, `u3`;
  `r = cellRadiusAt(theta) x margin - d`; accepted if `r > 0`, `u3 rMax < r` (area element) and outside the nucleus
  footprint (`nucleusColumnLocal` null). `rMax` = max of `cellRadiusAt` over 512 angles x margin (geometry cache).
  128 tries, then the last candidate.
- **Pick** (`mtPickEnd`): `MT_END_CANDIDATES` (12) ends in sequence, then one draw `u`; weight
  `exp(mtDirKappa (cos a - 1))`, `a` between start -> end and nucleus centre -> start in xy (cos = 0 if either is
  shorter than 1e-9); the first candidate whose running weight sum exceeds `u x sum`.
- Draw order per microtubule (one `hashStream`, as before): start tries, end tries x 12, pick, `fracEnd`, then the
  walk. The start z is the sampled one (`fracStart = z / ceiling`).
- `goOverNucleus` (step above) uses the sampled start z.
- Removed: `buildMtDirectionTable`, `mtSampleDirection`, `mtRayCellBoundaryFromNucleus`, `mtNucleusRadiusAt`,
  `MT_N_DIR`, `MT_MARCH_STEPS`, `MT_BISECT_ITERS`, the geometry cache's `dirTable`.
- Params: removed `mtStartFracMin/Max`, `mtStartOffsetXY`, `mtEndFracMin/Max`, `mtEndJitterDeg`; new:

  | Param | Default | Slider | Meaning |
  |---|---|---|---|
  | `mtStartDecayPct` | 1.6 | 0.05..10 | start density falls off as exp(-distance to the nucleus / L), L = this % of the equivalent diameter |
  | `mtEndDecayPct` | 20 | 0.5..50 | end density falls off as exp(-distance to the edge / L), same L convention |
  | `mtDirKappa` | 1.5 | 0..10 | weight exp(kappa (cos a - 1)) of the end's direction; 0 = any |

- Port: the param table, cli/viewer options, `tests/parity/js_reference.mjs` (`PARAM_KEYS`, the min/max pairs in
  `normalise`) and `tests/parity/make_cases.mjs` (`mtsparse`'s `mtStartOffsetXY`, `mtvar`'s `mtEndJitterDeg`).
- `web/tools/check_cellfield_microtubules.mjs`: scenarios use the new params; its nucleus test is now
  `nucleusColumnLocal` (the old centred-ellipsoid test was wrong for the shaped nucleus).

## Outside the prototype (JS only)

- `web/index.html` (the viewer):
  - Nucleus panel: Gap below, Irregularity, Kidney bend and Widest point pairs and Top/bottom asym., plus Lobe
    smoothness and Height irreg. (advanced).
  - `params()` gains the 11 new keys; the current WASM ignores them (`nonCore`).
  - Microtubules panel: Start near nucleus, End near edge, Direction focus replace Start offset, Start XY jitter,
    End offset and End dir. jitter.
  - `drawNucleus(cell, asset)` draws `asset.nuc` (surface rings from the engine's `cell` reply) when present,
    else the old ellipsoid from the pack record.
  - View panel: an x–z checkbox at the end of the Tilt line shows a side view (x–z, seen along y, equal x and z
    scale) along the bottom 20 % of the canvas: cytoplasm envelope, nuclei, microtubules and dyes, cut to the
    view's y range (`drawXZ`). Display only; works on the current WASM too (ellipsoid nuclei). The port mentions it
    in `docs/try-viewer.md`. The nucleus is drawn from the engine's rings, which reach both poles (no caps).
  - The ellipsoid fallback draws 17 sections (was 9).
- `web/lab/engine.js`: the `cell` reply gains `nuc` (Float64Array of rings x 48 points x xyz, cell-local; the
  `nucleusRingsLocal(c, 17, 48)` sections) and `nucPts`.
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
  - add the 11 params to the param table (`isc_params_set`);
  - the bend and the recurrences use only + - * / sqrt, and the spectrum uses `jsm::exp`/`jsm::log`.
- ABI: a nucleus-ring query for the viewer's `cell` job (`nuc`, `nucPts`), or the viewer draws nothing new.
  The ellipsoid fallback stays for the pack record.
- Optical volume (ABI 6): the asymmetric chord from the shaped nucleus.
- Adapter: `SimType_CellFieldNucIrregMin/Max`, `NucBendMin/Max`, `NucSmooth`, `NucThickIrreg`, `NucAsym`,
  `NucWidestMin/Max`, `NucBaseMin/Max`, or
  whatever the CellField param bridge exposes. cli: the matching options.
- Re-freeze `spec/golden`, because the defaults change every cell's shape and height.
- Remove `cellHeightMin/Max` from the core's param table, the adapter (`SimType_CellField...` height properties,
  if any) and the cli/docs/gallery, and from `tests/parity/js_reference.mjs` (its `PARAM_KEYS`/`normalise` list
  them and throw on a missing page default; it also needs the new nucleus params).
- Update `spec/ALGORITHM.md` and `docs/physics` (cell model), and add a gallery entry.
- `web/lab/engine_check.mjs` fails until then (lab engine vs main's WASM: shapes differ at the defaults).
