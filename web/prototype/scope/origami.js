// DNA-origami nanorulers (specimen 1, data/specimens.json): rods lying on a coverslip, each a design of
// data/origami/designs.json (the 80 nm nanoruler: a 200 x 13 nm 12-helix bundle with three spots of docking strands),
// bent in-plane as a 2D surface-equilibrated worm-like chain. The JS reference of core/src/origami.cpp (the port);
// the model is tools/origami_reference/origami_bend.py (BentShape, binding_sites) with its sequential numpy stream
// replaced by address-based pcg4d draws. BSD-3-Clause.
//
// Per molecule: rest sites -> incorporation (the label's LABEL/FLUOR draws, as on microtubules) -> deformation (wlc2d
// or rigid) -> landing (roll about the rod's axis, attach height) -> placement (position, yaw) -> sites in world um.
// Every step is a pure function of (seed, tile, molecule index): placement draws never depend on the bending
// parameters, so changing those never moves a molecule.
import { pcg4d, pcgA, unit } from './rng.js';
import { DYE_CH } from './dyes.js';
import { ORIGAMI_DESIGNS } from './origami_designs_data.js';
export { ORIGAMI_DESIGNS };

export const SPECIMEN_CELLFIELD = 0;
export const SPECIMEN_NANORULERS = 1;
export const STRUCTURE_ORIGAMI = 1;   // ISC_STRUCT_ORIGAMI_SITES: the docking strands' target (data/specimens.json)
// The field is tiled in ORIGAMI_TILE_UM squares; a tile holds Poisson(density x area) candidates (capped), thinned
// against the 3x3 tiles around it (Matern type II): a candidate is dropped when one of higher priority lies closer
// than origamiMinSpacingNm (capped at the tile size, so 3x3 tiles suffice).
export const ORIGAMI_TILE_UM = 2.0;
export const ORIGAMI_MAX_PER_TILE = 4096;
export const ORIGAMI_SALT = 0x4F524947;       // 'ORIG': placement (seed ^ salt, tx, ty, channel)
export const ORIGAMI_DYE_SALT = 0x52554C52;   // 'RULR': the molecule hash h1 = pcg4d(seed ^ salt, tx, ty, i).a
// Placement channels (4th word): the tile's count, then candidate i at CAND0 + i (x, y, priority, yaw = its 4 words).
export const ORIGAMI_CH = { COUNT: 0, CAND0: 16 };
// Molecule draws on h1 (2nd word above any spot index k, so they never meet a site's (h1, k, n, ch) draws): the
// bending normals pcg4d(h1, K_BEND, pair, 0) (Box-Muller: segments 2 pair and 2 pair + 1), the roll pcg4d(h1, K_LAND, 0, 0).
export const ORIGAMI_K_BEND = 0x80000000;
export const ORIGAMI_K_LAND = 0x80000001;
export const ORIGAMI_MIN_SEGMENTS = 8;

// The core params of the specimen (core/src/params.cpp kFields; the engine's origami-* options and p.<name>).
// Defaults: the 80 nm nanoruler's suggestions (designs.json) and estimates (docs/samples/nanorulers).
export const ORIGAMI_DEFAULTS = {
  specimen: 0,                  // 0 CellField, 1 Nanorulers
  origamiDesign: 0,             // index into data/origami/designs.json
  origamiDensity: 1.2,          // molecules per um^2 before thinning (estimate, 2026-10-12; was 0.3)
  origamiMinSpacingNm: 0,       // Matern-II hard core between molecule centres (0 = none; 250 until 2026-10-12)
  origamiBend: 1,               // 1 = the design's deformation, 0 = rigid
  origamiLpNm: 1500,            // persistence length, nm (estimate; measured bundles: chhabra2020)
  origamiStrandsPerSpot: 5,     // ring positions 0..N-1 of each spot carry a docking strand (1..ring n)
  origamiAttachNm: 7,           // the body's bottom above the glass, nm (biotin-streptavidin-BSA; estimate)
  origamiLinkerNm: 3,           // docking-strand linker: the dye uniform in an outward half-ball of this radius (estimate)
  origamiRoll: 0,               // 0 uniform roll about the axis, 1 fixed at origamiRollDeg
  origamiRollDeg: 0,
  origamiHeights: 0,            // 0 = 2D: every site at the axis height; 1 = keep the ring's heights (3D)
};
export const ORIGAMI_PARAM_NAMES = Object.keys(ORIGAMI_DEFAULTS);

// NormalizeParams for the specimen's params (clamped, integers truncated); unknown keys pass through.
export function normalizeOrigamiParams(p) {
  const q = { ...ORIGAMI_DEFAULTS, ...p };
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  q.specimen = Math.trunc(clamp(+q.specimen, 0, 1));
  q.origamiDesign = Math.trunc(clamp(+q.origamiDesign, 0, ORIGAMI_DESIGNS.length - 1));
  const d = ORIGAMI_DESIGNS[q.origamiDesign];
  q.origamiDensity = clamp(+q.origamiDensity, 0, 50);
  q.origamiMinSpacingNm = clamp(+q.origamiMinSpacingNm, 0, ORIGAMI_TILE_UM * 1000);
  q.origamiBend = +q.origamiBend !== 0 ? 1 : 0;
  q.origamiLpNm = clamp(+q.origamiLpNm, 1, 1e12);
  q.origamiStrandsPerSpot = Math.trunc(clamp(+q.origamiStrandsPerSpot, 0, d.maxRingN));
  q.origamiAttachNm = clamp(+q.origamiAttachNm, 0, 1000);
  q.origamiLinkerNm = clamp(+q.origamiLinkerNm, 0, 100);
  q.origamiRoll = +q.origamiRoll !== 0 ? 1 : 0;
  q.origamiHeights = +q.origamiHeights !== 0 ? 1 : 0;
  return q;
}

// ---- molecule shape (origami_bend.py BentShape, frame "aligned") ----

// Displacement after arc length s from angle theta0 with curvature kappa (an exact circular arc; _arc).
function arc(theta0, kappa, s) {
  const half = kappa * s / 2.0;
  const chord = half === 0 ? s : s * (Math.sin(half) / half);
  const mid = theta0 + half;
  return [chord * Math.cos(mid), chord * Math.sin(mid)];
}

// Standard normal i of a molecule's bending (Box-Muller on one pcg4d call per pair of segments).
export function bendNormal(h1, i) {
  const w = pcg4d(h1 >>> 0, ORIGAMI_K_BEND, i >>> 1, 0);
  const r = Math.sqrt(-2.0 * Math.log(unit(w[0])));
  const t = 2.0 * Math.PI * unit(w[1]);
  return (i & 1) ? r * Math.sin(t) : r * Math.cos(t);
}

// One bent realisation of design d for molecule hash h1: the centreline as n circular arcs of length ds, in the
// "aligned" frame (end-to-end chord along +x, chord midpoint at the origin; a straight shape maps to itself).
// normals: optional array of n standard normals (the reference cross-check feeds origami_bend.py's).
export function bentShape(d, P, h1, normals = null) {
  const L = d.lengthNm, W = d.diameterNm;
  const n = Math.max(ORIGAMI_MIN_SEGMENTS, Math.ceil(L / d.segmentNm));
  const ds = L / n;
  const bend = P.origamiBend !== 0 && d.deformation === 'wlc2d';
  const sd = bend ? Math.sqrt(ds / P.origamiLpNm) : 0.0;
  const theta = new Float64Array(n + 1), kappa = new Float64Array(n);
  const nx = new Float64Array(n + 1), ny = new Float64Array(n + 1);
  let maxAbsKappa = 0.0;
  for (let i = 0; i < n; i++) {
    const z = bend ? (normals ? normals[i] : bendNormal(h1, i)) : 0.0;
    const dtheta = z * sd;
    kappa[i] = dtheta / ds;
    theta[i + 1] = theta[i] + dtheta;
    maxAbsKappa = Math.max(maxAbsKappa, Math.abs(kappa[i]));
  }
  for (let i = 0; i < n; i++) {
    const [dx, dy] = arc(theta[i], kappa[i], ds);
    nx[i + 1] = nx[i] + dx;
    ny[i + 1] = ny[i] + dy;
  }
  const phi = -Math.atan2(ny[n] - ny[0], nx[n] - nx[0]);
  return { L, W, n, ds, theta, kappa, nx, ny, phi, cphi: Math.cos(phi), sphi: Math.sin(phi),
    ax: (nx[0] + nx[n]) / 2, ay: (ny[0] + ny[n]) / 2,
    endAngle: theta[n] - theta[0], fold: maxAbsKappa * W / 2 >= 1 };
}

// Bent centreline at straight coordinate x (nm, origin at the body's centre): [X, Y, tangent angle].
export function centerline(S, x) {
  const u = x + S.L / 2;
  const i = Math.min(S.n - 1, Math.max(0, Math.floor(u / S.ds)));
  const s = u - i * S.ds;   // may run past an end: extrapolates
  const th0 = S.theta[i], k = S.kappa[i];
  const [dx, dy] = arc(th0, k, s);
  const qx = S.nx[i] + dx - S.ax, qy = S.ny[i] + dy - S.ay;
  return [S.cphi * qx - S.sphi * qy, S.sphi * qx + S.cphi * qy, th0 + k * s + S.phi];
}

// Straight in-plane (x, y) -> bent (X, Y), nm (Euler-Bernoulli: cross-sections stay normal to the centreline).
export function transform(S, x, y) {
  const [X, Y, th] = centerline(S, x);
  return [X - y * Math.sin(th), Y + y * Math.cos(th)];
}

// Closed top-view outline of the bent body, [x0, y0, x1, y1, ...] nm (BentShape.outline).
export function shapeOutline(S, nPerSide = 64) {
  const L = S.L, W = S.W, nEnd = Math.max(2, nPerSide >> 2), out = [];
  const push = (x, y) => { const [X, Y] = transform(S, x, y); out.push(X, Y); };
  for (let j = 0; j < nPerSide; j++) push(-L / 2 + L * j / (nPerSide - 1), W / 2);
  for (let j = 0; j < nEnd; j++) push(L / 2, W / 2 - W * j / (nEnd - 1));
  for (let j = 0; j < nPerSide; j++) push(L / 2 - L * j / (nPerSide - 1), -W / 2);
  for (let j = 0; j < nEnd; j++) push(-L / 2, -W / 2 + W * j / (nEnd - 1));
  return out;
}

export const moleculeH1 = (seed, tx, ty, i) => pcgA((seed ^ ORIGAMI_DYE_SALT) >>> 0, tx >>> 0, ty >>> 0, i >>> 0);

// The roll of a molecule about its own axis, radians (landing: uniform per molecule, or fixed).
export function moleculeRoll(d, P, h1) {
  if (P.origamiRoll !== 0) return P.origamiRollDeg * Math.PI / 180.0;
  return 2.0 * Math.PI * unit(pcgA(h1 >>> 0, ORIGAMI_K_LAND, 0, 0));
}

// The docking strands of a placed molecule m (fieldMolecules' entry), fluorescent under the label (density =
// incorporation, fluorescentFraction): appends {k, n, id, rest: [x, y, h] nm (straight frame, h from the axis),
// bent: [X, Y] nm (molecule frame), x, y, z (world um)}. withAll: every designed strand, label draws ignored (drawing).
export function moleculeSites(d, P, m, S, density, fluorescentFraction, out = [], withAll = false) {
  const labelledBelow = Math.min(1.0, Math.max(0.0, density));
  const ff = Math.min(1.0, Math.max(0.0, fluorescentFraction));
  if (!withAll && (!(labelledBelow > 0) || !(ff > 0))) return out;
  const h1 = m.h1, roll = moleculeRoll(d, P, h1), Lk = P.origamiLinkerNm;
  const cy = Math.cos(m.yaw), sy = Math.sin(m.yaw);
  const zAxisNm = P.origamiAttachNm + d.diameterNm / 2;
  for (let k = 0; k < d.spots.length; k++) {
    const sp = d.spots[k];
    const nOn = Math.min(sp.ringN, P.origamiStrandsPerSpot);
    for (let n = 0; n < nOn; n++) {
      const label = pcgA(h1, k, n, DYE_CH.LABEL);
      if (!withAll) {
        if (unit(label) >= labelledBelow) continue;
        if (ff < 1.0 && unit(pcgA(h1, k, n, DYE_CH.FLUOR)) >= ff) continue;
      }
      const a = roll + sp.phaseDeg * Math.PI / 180.0 + 2.0 * Math.PI * n / sp.ringN;
      const ca = Math.cos(a), sa = Math.sin(a);
      // Linker: the dye uniform in the outward half-ball of radius Lk on the surface point (cos of the angle to the
      // surface normal uniform in [0, 1), azimuth uniform, radius Lk cbrt(u)).
      const cu = unit(pcgA(h1, k, n, DYE_CH.LINK_U));
      const ph = 2.0 * Math.PI * unit(pcgA(h1, k, n, DYE_CH.LINK_PHI));
      const rr = Lk * Math.cbrt(unit(pcgA(h1, k, n, DYE_CH.LINK_R)));
      const sn = Math.sqrt(1.0 - cu * cu);
      const dn = rr * cu, d1 = rr * sn * Math.cos(ph), d2 = rr * sn * Math.sin(ph);
      // Frame at the surface point: normal (0, ca, sa), along the axis (1, 0, 0), around it (0, -sa, ca).
      const x = sp.atNm - d.lengthNm / 2 + d1;
      const y = sp.radiusNm * ca + dn * ca - d2 * sa;
      const h = sp.radiusNm * sa + dn * sa + d2 * ca;
      const [X, Y] = transform(S, x, y);
      const zNm = P.origamiHeights !== 0 ? zAxisNm + h : zAxisNm;
      out.push({ k, n, id: label, rest: [x, y, h], bent: [X, Y],
        x: m.x + (X * cy - Y * sy) / 1000.0, y: m.y + (X * sy + Y * cy) / 1000.0, z: zNm / 1000.0 });
    }
  }
  return out;
}

// ---- the field ----

function poissonInverse(m, u) {
  let p = Math.exp(-m), F = p, c = 0;
  while (u > F && c < ORIGAMI_MAX_PER_TILE) { c++; p *= m / c; F += p; }
  return c;
}

// The candidates of tile (tx, ty): [{tx, ty, i, x, y (world um), prio (uint32), yaw}].
export function tileCandidates(seed, P, tx, ty) {
  const s = (seed ^ ORIGAMI_SALT) >>> 0;
  const lambda = P.origamiDensity * ORIGAMI_TILE_UM * ORIGAMI_TILE_UM;
  const count = lambda > 0 ? poissonInverse(lambda, unit(pcgA(s, tx >>> 0, ty >>> 0, ORIGAMI_CH.COUNT))) : 0;
  const out = [];
  for (let i = 0; i < count; i++) {
    const w = pcg4d(s, tx >>> 0, ty >>> 0, (ORIGAMI_CH.CAND0 + i) >>> 0);
    out.push({ tx, ty, i, x: (tx + unit(w[0])) * ORIGAMI_TILE_UM, y: (ty + unit(w[1])) * ORIGAMI_TILE_UM, prio: w[2],
      yaw: 2.0 * Math.PI * unit(w[3]) });
  }
  return out;
}

// a beats b (a higher priority; ties by address, the later address winning).
const beats = (a, b) => a.prio !== b.prio ? a.prio > b.prio
  : a.ty !== b.ty ? a.ty > b.ty : a.tx !== b.tx ? a.tx > b.tx : a.i > b.i;

export class OrigamiField {
  constructor(seed, params) {
    this.seed = seed >>> 0;
    this.P = normalizeOrigamiParams(params);
    this.d = ORIGAMI_DESIGNS[this.P.origamiDesign];
    this.cands = new Map();
    this.tiles = new Map();
    this.shapes = new Map();
  }
  candidates(tx, ty) {
    const key = tx + ',' + ty;
    let c = this.cands.get(key);
    if (!c) { c = tileCandidates(this.seed, this.P, tx, ty); this.cands.set(key, c); }
    return c;
  }
  // The molecules of a tile after thinning, in candidate order: [{tx, ty, i, x, y, yaw, prio, h1}].
  tileMolecules(tx, ty) {
    const key = tx + ',' + ty;
    let mols = this.tiles.get(key);
    if (mols) return mols;
    const r = this.P.origamiMinSpacingNm / 1000.0, r2 = r * r;
    const own = this.candidates(tx, ty);
    const near = [];
    if (r > 0) for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) near.push(this.candidates(tx + dx, ty + dy));
    mols = [];
    for (const c of own) {
      let kept = true;
      for (const list of near) {
        for (const o of list) {
          if (o === c) continue;
          const ex = o.x - c.x, ey = o.y - c.y;
          if (ex * ex + ey * ey < r2 && beats(o, c)) { kept = false; break; }
        }
        if (!kept) break;
      }
      if (kept) mols.push({ ...c, h1: moleculeH1(this.seed, tx, ty, c.i) });
    }
    this.tiles.set(key, mols);
    return mols;
  }
  // How far a molecule's sites reach from its centre (the chord midpoint), um: no point of a curve of length L lies
  // farther than L/2 from its chord's midpoint, plus the cross-section and the linker.
  reachUm() { return (this.d.lengthNm / 2 + this.d.diameterNm / 2 + this.P.origamiLinkerNm) / 1000.0; }
  maxHeightUm() { return (this.P.origamiAttachNm + this.d.diameterNm + this.P.origamiLinkerNm) / 1000.0; }
  // Molecules whose reach meets [x0,x1) x [y0,y1), tile by tile (tx outer, ty inner), in molecule order.
  moleculesInRect(x0, y0, x1, y1, extraUm = 0.0) {
    const reach = this.reachUm() + extraUm, T = ORIGAMI_TILE_UM;
    const tx0 = Math.floor((x0 - reach) / T), tx1 = Math.floor((x1 + reach) / T);
    const ty0 = Math.floor((y0 - reach) / T), ty1 = Math.floor((y1 + reach) / T);
    const out = [];
    for (let tx = tx0; tx <= tx1; tx++)
      for (let ty = ty0; ty <= ty1; ty++)
        for (const m of this.tileMolecules(tx, ty)) {
          const dx = Math.max(Math.max(x0 - m.x, 0.0), m.x - x1), dy = Math.max(Math.max(y0 - m.y, 0.0), m.y - y1);
          if (Math.hypot(dx, dy) <= reach) out.push(m);
        }
    return out;
  }
  shape(m) {
    const key = m.tx + ',' + m.ty + ',' + m.i;
    let S = this.shapes.get(key);
    if (!S) { S = bentShape(this.d, this.P, m.h1); this.shapes.set(key, S); }
    return S;
  }
  sites(m, density, fluorescentFraction, withAll = false) {
    return moleculeSites(this.d, this.P, m, this.shape(m), density, fluorescentFraction, [], withAll);
  }
  // isc_origami_in_window's row: tx, ty, i, design, x, y (um), yaw, roll (rad), end angle (rad), fold (0/1).
  info(m) {
    const S = this.shape(m);
    return { tx: m.tx, ty: m.ty, i: m.i, design: this.P.origamiDesign, x: m.x, y: m.y, yaw: m.yaw,
      roll: moleculeRoll(this.d, this.P, m.h1), endAngle: S.endAngle, fold: S.fold ? 1 : 0 };
  }
  // Bent outline in world um, [x, y, ...].
  outlineWorld(m, nPerSide = 64) {
    const o = shapeOutline(this.shape(m), nPerSide), cy = Math.cos(m.yaw), sy = Math.sin(m.yaw), out = new Array(o.length);
    for (let j = 0; j < o.length; j += 2) {
      out[j] = m.x + (o[j] * cy - o[j + 1] * sy) / 1000.0;
      out[j + 1] = m.y + (o[j] * sy + o[j + 1] * cy) / 1000.0;
    }
    return out;
  }
}
