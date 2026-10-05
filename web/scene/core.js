// SPDX-License-Identifier: BSD-3-Clause
// The viewer's scene core: the camera, the structure/layer registry, the themes and the packing of per-layer
// clip intervals into the renderer's uniform tables. DOM-free (a classic script that sets globalThis.IscScene),
// so Node tests load it as it is (tests/web/scene_core_check.mjs).
//
// Camera: orthographic, about a pivot (px, py, pz). The view frame is the world turned by -az about the
// vertical through the pivot, then tilted about its horizontal axis:
//   u = cos az dx + sin az dy                 (screen x / S)
//   v = -sin az dx + cos az dy                (the turned y)
//   screen y / S = v cos t - (z - pz) sin t,  depth (nearness) = v sin t + (z - pz) cos t
// At az = 0, pz = 0 this is exactly the viewer's original oblique projection (x unchanged, y foreshortened by
// cos t, z lifting the point by sin t). A cell's local frame (packRot) composes with the turn into one
// effective rotation packRot - az, so every per-cell cache keyed on packRot works unchanged on that angle.
'use strict';
(function () {
const DEG = Math.PI / 180;

// spec: {px, py, pz, az, tilt (radians), S (device px per um), W, H (device px)}
function makeCamera(spec) {
  const az = spec.az || 0, tilt = spec.tilt || 0;
  const ca = Math.cos(az), sa = Math.sin(az), ct = Math.cos(tilt), st = Math.sin(tilt);
  const px = spec.px, py = spec.py, pz = spec.pz || 0, S = spec.S, W2 = spec.W / 2, H2 = spec.H / 2;
  const cam = {
    az, tilt, ca, sa, ct, st, px, py, pz, S, W: spec.W, H: spec.H, W2, H2,
    // rows of the world -> view rotation (u, screen-y, depth), for shaders and tests
    r0: [ca, sa, 0], r1: [-sa * ct, ca * ct, -st], r2: [-sa * st, ca * st, ct],
    // turned-frame offset of a world point from the pivot
    u(x, y) { return ca * (x - px) + sa * (y - py); },
    v(x, y) { return -sa * (x - px) + ca * (y - py); },
    project(x, y, z) {
      const dx = x - px, dy = y - py, u = ca * dx + sa * dy, v = -sa * dx + ca * dy;
      return [W2 + u * S, H2 + (v * ct - (z - pz) * st) * S];
    },
    depth(x, y, z) { const v = -sa * (x - px) + ca * (y - py); return v * st + (z - pz) * ct; },
    // the effective rotation of a cell's local frame in the view frame
    cellRot(packRot) { return (packRot || 0) - az; },
    // world point at screen (sx, sy) on the plane z (tilt clamped like the view extent)
    unproject(sx, sy, z) {
      const u = (sx - W2) / S, v = ((sy - H2) / S + ((z || 0) - pz) * st) / Math.max(0.15, ct);
      return [px + ca * u - sa * v, py + sa * u + ca * v];
    },
    // half extents of the view in the turned frame (um): the visible v range widens with the tilt
    halfU() { return W2 / S; },
    halfV() { return H2 / S / Math.max(0.15, ct); },
    // world-axis-aligned box [x0, y0, x1, y1] holding the view's turned rectangle
    visibleRect() {
      const hu = W2 / S, hv = H2 / S / Math.max(0.15, ct);
      const ex = Math.abs(ca) * hu + Math.abs(sa) * hv, ey = Math.abs(sa) * hu + Math.abs(ca) * hv;
      return [px - ex, py - ey, px + ex, py + ey];
    },
    // world displacement of the pivot for a drag of (dx, dy) device px (the scene follows the pointer)
    panDelta(dx, dy) {
      const du = -dx / S, dv = -dy / (S * Math.max(0.15, ct));
      return [ca * du - sa * dv, sa * du + ca * dv];
    },
  };
  return cam;
}

// ---- structures and layers ----------------------------------------------------------------------------------------
// A structure has geometry representations (the simulated ground truth) and, when it carries a dye population, the
// data representations made from imaging those dyes. A layer id is 'structure.rep'. New structures (issue 15:
// nuclear pores on the envelope; issue 16: one dye per structure) are one entry here plus their renderer primitive.
const REPS = {
  surface:  { label: 'surface', prim: 'mesh', group: 'geometry' },
  outline:  { label: 'outline', prim: 'mesh', group: 'geometry' },
  contours: { label: 'height contours', prim: 'mesh', group: 'geometry' },
  lines:    { label: 'simulated', prim: 'mtRibbon', group: 'geometry' },
  dyes:     { label: 'dye sites', prim: 'points', group: 'geometry', needs: 'dyes' },
  wfSlice:  { label: 'WideField slice', prim: 'slice', group: 'data', needs: 'stack:wf', followsPlane: true },
  wfIso:    { label: 'WideField thresholded', prim: 'iso', group: 'data', needs: 'stack:wf' },
  srFrames: { label: 'SMLM camera frames', prim: 'slice', group: 'data', needs: 'stack:sr', followsPlane: true },
  locs:     { label: 'SMLM localizations', prim: 'points', group: 'data', needs: 'events' },
  bfSlice:  { label: 'BrightField slice', prim: 'slice', group: 'data', needs: 'stack:bf', followsPlane: true },
};
const DYE_REPS = ['dyes', 'wfSlice', 'wfIso', 'srFrames', 'locs'];
const STRUCTURES = [
  { id: 'mt', label: 'Microtubules', reps: ['lines'], dyes: 'mt' },
  { id: 'nucleus', label: 'Nucleus', reps: ['surface'] },
  { id: 'cyto', label: 'Cytoplasm', reps: ['surface', 'outline', 'contours'] },
  { id: 'cell', label: 'Whole cell', reps: ['bfSlice'] },   // BrightField images every structure at once
];
// Representations the renderer draws today; the other layers are listed but marked not available yet.
const IMPLEMENTED = new Set(['surface', 'outline', 'contours', 'lines', 'dyes', 'wfSlice', 'srFrames', 'bfSlice']);
// GL kinds: the index of a layer in the renderer's clip tables (mesh kinds 0-3 share one draw per cell).
const KIND = { 'cyto.surface': 0, 'nucleus.surface': 1, 'cyto.outline': 2, 'cyto.contours': 3, 'mt.lines': 4, 'mt.dyes': 5 };
const N_KINDS = 6, IV_MAX = 4;

function buildLayers(structures) {
  const out = new Map();
  for (const s of structures || STRUCTURES) {
    const reps = s.reps.concat(s.dyes ? DYE_REPS : []);
    for (const rep of reps) {
      const R = REPS[rep], id = s.id + '.' + rep;
      out.set(id, { id, structure: s.id, rep, label: s.label + ' ' + R.label, prim: R.prim, group: R.group,
        needs: R.needs || null, followsPlane: !!R.followsPlane, kind: id in KIND ? KIND[id] : -1,
        implemented: IMPLEMENTED.has(rep) });
    }
  }
  return out;
}
const LAYERS = buildLayers(STRUCTURES);
// The registry the animation editor and evaluator read: layers by id, and per structure its label, layers and primary
// (ground-truth geometry) rep, what a cycle's '$.gt' means.
const REGISTRY = { layers: LAYERS, structures: new Map(STRUCTURES.map(s => [s.id, { id: s.id, label: s.label, primary: s.reps[0],
  dyes: !!s.dyes, layers: [...LAYERS.values()].filter(L => L.structure === s.id).map(L => L.id) }])) };

// ---- themes ---------------------------------------------------------------------------------------------------------
// color: [r, g, b] 0-255, or null = the renderer's own colouring (depth colours for surfaces). opacity 0-1. width
// (um, microtubules) and size (device px, points) are per layer.
const THEMES = {
  viewer: { label: 'Viewer (depth colours)', bg: [5, 7, 10], layers: {
    'cyto.surface': { color: null, opacity: 0.6 },
    'nucleus.surface': { color: null, opacity: 0.5 },
    'cyto.outline': { color: [90, 169, 230], opacity: 0.25 },
    'cyto.contours': { color: [20, 30, 45], opacity: 0.55 },
    'mt.lines': { color: [230, 198, 90], opacity: 0.8 },
    'mt.dyes': { color: [255, 77, 77], opacity: 1, size: 1.2 },
    'mt.wfSlice': { color: [255, 255, 255], opacity: 0.9, blend: 'alpha', gamma: 1 },
    'mt.srFrames': { color: [255, 255, 255], opacity: 0.9, blend: 'alpha', gamma: 1 },
    'cell.bfSlice': { color: [255, 255, 255], opacity: 0.85, blend: 'alpha', gamma: 1 },
  }, ghost: { color: null, opacity: 0.25 } },
  fluo: { label: 'Dark fluorescence', bg: [0, 0, 0], layers: {
    'cyto.surface': { color: [38, 70, 120], opacity: 0.3 },
    'nucleus.surface': { color: [70, 110, 255], opacity: 0.4 },
    'cyto.outline': { color: [110, 170, 255], opacity: 0.55 },
    'cyto.contours': { color: [70, 100, 150], opacity: 0.4 },
    'mt.lines': { color: [80, 255, 225], opacity: 1 },
    'mt.dyes': { color: [255, 90, 210], opacity: 1, size: 1.4 },
    'mt.wfSlice': { color: [255, 225, 110], opacity: 1, blend: 'add', gamma: 0.6 },
    'mt.srFrames': { color: [255, 220, 120], opacity: 1, blend: 'add', gamma: 1 },
    'cell.bfSlice': { color: [235, 235, 235], opacity: 0.85, blend: 'alpha', gamma: 1 },
  }, ghost: { color: [60, 90, 140], opacity: 0.18 } },
};
function themeOf(name) { return THEMES[name] || THEMES.viewer; }
function hexToRgb(c) {
  if (Array.isArray(c) || c === null || c === undefined) return c;
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(c));
  return m ? [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)] : null;
}
// theme default < layer style < interval style; 'color' may be '#rrggbb', [r, g, b] or null
function resolveStyle(theme, id, ...over) {
  const out = Object.assign({}, themeOf(theme).layers[id] || { color: null, opacity: 1 });
  for (const o of over) if (o) for (const k in o) if (o[k] !== undefined) out[k] = k === 'color' ? hexToRgb(o[k]) : o[k];
  return out;
}

// ---- clip intervals -> uniform tables ------------------------------------------------------------------------------
// state.layers: { id: { style?, alpha?, intervals?: [{lo, hi, style?}] } }, absent = hidden; no intervals = everywhere;
// alpha multiplies every interval's opacity (fades).
// Returns {n: Float32Array(N_KINDS) interval counts (0 = hidden), iv: Float32Array(N_KINDS*IV_MAX*2) [lo, hi],
// st: Float32Array(N_KINDS*IV_MAX*4) rgb 0-1 (r < 0: the renderer's own colour) + opacity, layer: per kind the
// resolved layer style (width/size)}. The intervals are along state.sweep's axis (absolute um); +-1e30 = open.
const BIG = 1e30;
function packZones(state) {
  const n = new Float32Array(N_KINDS), iv = new Float32Array(N_KINDS * IV_MAX * 2), st = new Float32Array(N_KINDS * IV_MAX * 4);
  const layer = new Array(N_KINDS).fill(null);
  const layers = state.layers || {};
  for (const id in KIND) {
    const k = KIND[id], L = layers[id];
    if (!L) continue;
    const base = resolveStyle(state.theme, id, L.style);
    layer[k] = base;
    const ivs = L.intervals && L.intervals.length ? L.intervals.slice(0, IV_MAX) : [{ lo: -BIG, hi: BIG }];
    n[k] = ivs.length;
    ivs.forEach((I, j) => {
      const s = I.style ? resolveStyle(state.theme, id, L.style, I.style) : base, o = k * IV_MAX + j;
      iv[2 * o] = Math.max(-BIG, I.lo === undefined ? -BIG : I.lo); iv[2 * o + 1] = Math.min(BIG, I.hi === undefined ? BIG : I.hi);
      const c = s.color;
      st[4 * o] = c ? c[0] / 255 : -1; st[4 * o + 1] = c ? c[1] / 255 : 0; st[4 * o + 2] = c ? c[2] / 255 : 0;
      st[4 * o + 3] = (s.opacity === undefined ? 1 : s.opacity) * (L.alpha === undefined ? 1 : L.alpha);
    });
  }
  return { n, iv, st, layer };
}
// The sweep axis as a world unit vector; 'z' | 'x' | 'y' | [nx, ny, nz].
function axisVector(sweep) {
  const a = sweep && sweep.axis;
  if (Array.isArray(a)) { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; }
  return a === 'x' ? [1, 0, 0] : a === 'y' ? [0, 1, 0] : [0, 0, 1];
}
// The axis in the camera's turned frame plus its offset, for s = dot(a', (u, v, z)) + w (absolute um along the axis):
// a' = (R(-az) a.xy, a.z), w = a.x px + a.y py.
function axisInView(cam, sweep) {
  const a = axisVector(sweep);
  return [cam.ca * a[0] + cam.sa * a[1], -cam.sa * a[0] + cam.ca * a[1], a[2], a[0] * cam.px + a[1] * cam.py];
}
// CPU twin of the shaders' test (2D fallback): the style slot index for value s of kind k, or -1.
function zoneSlot(Z, k, s) {
  for (let j = 0; j < Z.n[k]; j++) { const o = 2 * (k * IV_MAX + j); if (s >= Z.iv[o] && s <= Z.iv[o + 1]) return k * IV_MAX + j; }
  return -1;
}

// ---- detail budget ---------------------------------------------------------------------------------------------------
// The N cells nearest the pivot (xy) get the detailed structures. A cell already in the set ranks with its distance
// times HYST, so two nearly equidistant cells do not swap on every pan step. An orbit (az) never changes the set.
const DETAIL_HYST = 0.85;
function detailSet(cells, px, py, N, prev) {
  const rank = cells.map(c => {
    const d = Math.hypot(c.x - px, c.y - py);
    return { key: c.cx + ',' + c.cy, d: prev && prev.has(c.cx + ',' + c.cy) ? d * DETAIL_HYST : d };
  });
  rank.sort((a, b) => a.d - b.d || (a.key < b.key ? -1 : 1));
  const order = rank.map(r => r.key);
  return { set: new Set(N > 0 ? order.slice(0, N) : order), order };
}

globalThis.IscScene = { DEG, makeCamera, REPS, STRUCTURES, LAYERS, REGISTRY, KIND, N_KINDS, IV_MAX, IMPLEMENTED, buildLayers, THEMES,
  themeOf, resolveStyle, hexToRgb, packZones, axisVector, axisInView, zoneSlot, detailSet, DETAIL_HYST, BIG };
})();
