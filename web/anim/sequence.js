// SPDX-License-Identifier: BSD-3-Clause
// Animation sequences (issue 11): the model, its versioned JSON format and the evaluator. DOM-free (a classic script
// that sets globalThis.IscAnimSeq; tests/web/anim_unit.mjs runs it in Node).
//
// A sequence is cycles of steps. A step lasts `duration` seconds and has
//   - a camera move: rotation (by the cycle's orbit rate unless given), tilt and zoom (`fit`: the cell's footprint
//     fills 1/fit of the frame), each from where the previous step ended to an optional target, with an easing;
//   - an optional plane sweep along an axis (world z, x, y, or the screen's horizontal / depth), from -> to (relative
//     to the target cell's extent along that axis, plus um), with a slab thickness;
//   - layers, each in one or more zones of the sweep: AHEAD (the plane has not reached it yet), AT (inside the slab),
//     BEHIND (the plane has passed: the wake). Without a sweep every listed layer is shown (pops in, fading over
//     fadeSec). Sweeping up, BEHIND is below the slab; sweeping down, above it.
// A cycle is bound to a structure: a layer ref '$.rep' means that structure's rep ('$.gt' its ground-truth geometry),
// so a duplicated cycle re-bound to another structure animates that one. `keepAfter` layers stay shown in later
// cycles (unless a later step lists them itself, or a later cycle drops them).
//
// evalCompiled(compileSequence(seq, ctx), t) is a pure function of t: the renderer's scene state (web/index.html
// renderFrame: camera, layers with clip intervals along the sweep axis, slice plane) plus the overlays (caption,
// readout) -- so preview, scrubbing and export are the same frames.
'use strict';
(function () {
const FORMAT = 'insiliscope-animation', VERSION = 1, BIG = 1e30;

// ---- easing on [0, 1] ----
const EASE = {
  linear: u => u,
  smooth: u => u * u * (3 - 2 * u),
  inOut: u => (u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2),
  in: u => u * u,
  out: u => 1 - (1 - u) * (1 - u),
  hold: u => (u >= 1 ? 1 : 0),
};
const ease = (name, u) => (EASE[name] || EASE.linear)(Math.min(1, Math.max(0, u)));

let idSeq = 0;
const newId = p => p + Date.now().toString(36).slice(-4) + (idSeq++).toString(36);
const clone = o => JSON.parse(JSON.stringify(o));

// ---- defaults and migration ----
function defaults() {
  return {
    format: FORMAT, version: VERSION, name: 'Animation',
    scene: { target: { mode: 'center', cell: null }, scope: 'ghosts', ghostOpacity: 0.25, theme: 'fluo', detailCells: 1, crop: true },
    camera: { az: 0, tilt: 20, fit: 1.15, center: { dx: 0, dy: 0, dz: 0 }, orbitDegPerSec: 0 },
    styles: {},
    // the data layers' z-stacks (web/index.html dataParams): focus steps, SMLM frames per plane, frame averaging,
    // crop to the cell, fresh or sequential SMLM planes, frames per plane of a localization acquisition
    data: { srFps: 10, stepUm: 0.2, srStepUm: 0.4, srFrames: 10, wfAverage: 4, crop: true, sequential: false, locFrames: 5000 },
    output: { format: 'mp4', width: 1920, height: 1080, fps: 30, quality: 'high', loop: 'loop',
      overlays: { scaleBar: true, captions: true, readout: false, legend: false },
      gif: { width: 480, fps: 20, dither: 'bayer4' } },
    cycles: [],
  };
}
// Fills what an older or hand-written file lacks; unknown fields are kept (a newer layer type survives a round trip).
function migrate(json) {
  const src = typeof json === 'string' ? JSON.parse(json) : clone(json || {});
  if (src.format && src.format !== FORMAT) throw new Error('not an inSiliScope animation (format ' + src.format + ')');
  const warnings = [];
  if ((src.version || 1) > VERSION) warnings.push('saved by a newer viewer (version ' + src.version + '): some parts may be ignored');
  const d = defaults(), out = Object.assign({}, d, src);
  for (const k of ['scene', 'camera', 'data', 'output']) out[k] = Object.assign({}, d[k], src[k] || {});
  out.scene.target = Object.assign({}, d.scene.target, (src.scene && src.scene.target) || {});
  out.camera.center = Object.assign({}, d.camera.center, (src.camera && src.camera.center) || {});
  out.output.overlays = Object.assign({}, d.output.overlays, (src.output && src.output.overlays) || {});
  out.output.gif = Object.assign({}, d.output.gif, (src.output && src.output.gif) || {});
  out.styles = Object.assign({}, src.styles || {});
  out.format = FORMAT; out.version = Math.max(VERSION, src.version || 1);
  out.cycles = (src.cycles || []).map(c => Object.assign({ id: newId('c'), name: 'Cycle', structure: 'mt', enabled: true,
    orbit: { deg: 0 }, keepAfter: [], dropKept: [] }, c, {
    steps: (c.steps || []).map(s => Object.assign({ id: newId('s'), name: '', duration: 3, layers: [] }, s, {
      layers: (s.layers || []).map(l => Object.assign({ zones: ['all'] }, l)) })) }));
  for (const c of out.cycles) if (!c.id) c.id = newId('c');
  return { seq: out, warnings };
}

// ---- layer refs ----
// '$.rep' -> structure.rep ('$.gt' -> the structure's ground-truth geometry rep); anything else is absolute.
function bind(ref, structure, registry) {
  if (!ref || ref.slice(0, 2) !== '$.') return ref;
  const rep = ref.slice(2);
  if (rep === 'gt') {
    const s = registry && registry.structures && registry.structures.get(structure);
    return structure + '.' + (s ? s.primary : 'lines');
  }
  return structure + '.' + rep;
}

// ---- positions along a sweep axis ----
// The axis a sweep runs along, at the camera rotation az: a world unit vector.
function axisVec(axis, az) {
  if (axis === 'x') return [1, 0, 0];
  if (axis === 'y') return [0, 1, 0];
  if (axis === 'h') return [Math.cos(az), Math.sin(az), 0];     // the screen's horizontal
  if (axis === 'd') return [-Math.sin(az), Math.cos(az), 0];    // the screen's depth (into the view: nearer = larger)
  return [0, 0, 1];
}
// The target's extent [lo, hi] along an axis (world um): z from the coverslip to the cell top; x / y from the outline;
// the screen axes from the footprint circle.
function extentAlong(axis, b, az) {
  if (axis === 'z' || !axis) return [b.z[0], b.z[1]];
  if (axis === 'x') return [b.x[0], b.x[1]];
  if (axis === 'y') return [b.y[0], b.y[1]];
  const a = axisVec(axis, az), c = a[0] * b.center[0] + a[1] * b.center[1];
  return [c - b.diam / 2, c + b.diam / 2];
}
function posOf(p, ext) {
  if (p === undefined || p === null) return ext[0];
  if (typeof p === 'number') return ext[0] + p * (ext[1] - ext[0]);
  const base = p.abs !== undefined ? p.abs : ext[0] + (p.rel === undefined ? 0 : p.rel) * (ext[1] - ext[0]);
  return base + (p.um || 0);
}

// ---- compile ----
// ctx: { registry: {layers: Map(id -> {implemented, group, prim, followsPlane}), structures: Map(id -> {primary})},
//        bounds: {x: [x0, x1], y: [y0, y1], z: [z0, z1], center: [x, y, z], diam} (the target cell, world um),
//        aspect: frame width / height, target: 'cx,cy' | null }
// Returns {duration, steps: [...], warnings}.
function compileSequence(seq, ctx) {
  ctx = ctx || {};
  const reg = ctx.registry || null, warnings = [];
  const b = ctx.bounds || { x: [-15, 15], y: [-15, 15], z: [0, 4], center: [0, 0, 2], diam: 30 };
  if (!ctx.bounds) warnings.push('no target cell yet: framing a 30 um default box');
  const aspect = ctx.aspect || 16 / 9;
  const cam0 = seq.camera || {};
  // fit -> horizontal field of view (um): the footprint (and, tilted, its height) fills 1/fit of the frame
  const fovOf = (fit, tiltDeg) => {
    const t = tiltDeg * Math.PI / 180, D = b.diam, Hz = b.z[1] - b.z[0];
    return Math.max(1e-3, fit) * Math.max(D, (D * Math.cos(t) + Hz * Math.sin(t)) * aspect);
  };
  let cam = { az: cam0.az || 0, tilt: cam0.tilt === undefined ? 20 : cam0.tilt, fit: cam0.fit || 1.15 };
  const center = cam0.center || {};
  const pivot = [b.center[0] + (center.dx || 0), b.center[1] + (center.dy || 0), b.center[2] + (center.dz || 0)];
  const steps = [], kept = new Map();   // kept: bound id -> style
  let t = 0, prevVisible = new Map();   // id -> {style, intervals, axis} at the previous step's end
  (seq.cycles || []).forEach((cy, ci) => {
    if (cy.enabled === false) return;
    for (const d of cy.dropKept || []) kept.delete(bind(d, cy.structure, reg));
    const cdur = (cy.steps || []).reduce((s, st) => s + Math.max(0.05, +st.duration || 0), 0);
    const orbit = cy.orbit || {};
    const rate = orbit.deg ? orbit.deg / Math.max(1e-6, cdur) : orbit.degPerSec ? orbit.degPerSec : (cam0.orbitDegPerSec || 0);
    (cy.steps || []).forEach((st, si) => {
      const dur = Math.max(0.05, +st.duration || 0), sc = st.camera || {};
      const from = { ...cam };
      const to = {
        az: sc.az && sc.az.to !== undefined && sc.az.to !== null ? +sc.az.to : sc.az && sc.az.by !== undefined && sc.az.by !== null ? from.az + +sc.az.by : from.az + rate * dur,
        tilt: sc.tilt && sc.tilt.to !== undefined && sc.tilt.to !== null ? Math.min(90, Math.max(0, +sc.tilt.to)) : from.tilt,
        fit: sc.fit && sc.fit.to !== undefined && sc.fit.to !== null ? Math.max(0.05, +sc.fit.to) : from.fit,
      };
      const azEase = sc.az && (sc.az.to !== undefined || sc.az.by !== undefined) ? (sc.az.ease || 'inOut') : 'linear';
      const sw = st.sweep && st.sweep.axis && st.sweep.axis !== 'none' ? st.sweep : null;
      let sweep = null;
      if (sw) {
        const slab = Math.max(0, +sw.slab || 0);
        const ext0 = extentAlong(sw.axis, b, from.az * Math.PI / 180);
        const p0 = posOf(sw.from, ext0), p1 = posOf(sw.to === undefined ? { rel: 1 } : sw.to, ext0), dir = p1 >= p0 ? 1 : -1;
        const ov = sw.overshoot !== undefined ? +sw.overshoot : slab / 2;
        sweep = { axis: sw.axis, from: p0 - dir * ov, to: p1 + dir * ov, slab, dir, ease: sw.ease || 'linear',
          // screen axes move with the camera: their positions are offsets from the target centre along the axis
          relToCentre: sw.axis === 'h' || sw.axis === 'd' };
        if (sweep.relToCentre) {
          const a = axisVec(sw.axis, from.az * Math.PI / 180), c = a[0] * b.center[0] + a[1] * b.center[1];
          sweep.from -= c; sweep.to -= c;
        }
      }
      const layers = [];
      for (const L of st.layers || []) {
        const id = bind(L.ref, cy.structure, reg);
        const info = reg && reg.layers && reg.layers.get(id);
        if (reg && !info) { warnings.push(`cycle ${ci + 1} step ${si + 1}: no layer ${id}`); continue; }
        if (info && info.implemented === false) { warnings.push(`cycle ${ci + 1} step ${si + 1}: ${id} is not available yet`); continue; }
        layers.push({ id, zones: L.zones && L.zones.length ? L.zones : ['all'], style: L.style || null, plane: L.plane || null,
          followsPlane: !!(info && info.followsPlane) });
      }
      // kept layers of earlier cycles, unless listed here
      const listed = new Set(layers.map(l => l.id));
      for (const [id, style] of kept) if (!listed.has(id)) layers.push({ id, zones: ['all'], style, kept: true });
      const fadeSec = st.fadeSec !== undefined ? Math.max(0, +st.fadeSec) : sweep ? 0 : 0.3;
      const nowIds = new Set(layers.map(l => l.id));
      const entering = new Set(layers.filter(l => !prevVisible.has(l.id)).map(l => l.id));
      const exiting = [...prevVisible].filter(([id]) => !nowIds.has(id)).map(([id, v]) => ({ id, ...v }));
      steps.push({ t0: t, t1: t + dur, ci, si, cycleId: cy.id, stepId: st.id, name: st.name || '', caption: st.caption || '',
        camFrom: from, camTo: to, ease: { az: azEase, tilt: (sc.tilt && sc.tilt.ease) || 'inOut', fit: (sc.fit && sc.fit.ease) || 'inOut' },
        sweep, layers, fadeSec, entering, exiting });
      // what is visible at this step's end (for the next step's fades)
      // (a layer counts as visible when one of its intervals overlaps the cell's extent along the sweep)
      prevVisible = new Map();
      const extEnd = sweep ? (sweep.relToCentre ? [-b.diam / 2, b.diam / 2] : extentAlong(sweep.axis, b, to.az * Math.PI / 180)) : null;
      for (const l of layers) {
        const iv = sweep ? zoneIntervals(l.zones, sweep, sweep.to) : null;
        if (!iv || iv.some(([a, c]) => c > extEnd[0] && a < extEnd[1])) prevVisible.set(l.id, { style: l.style, intervals: iv, axis: sweep ? sweep.axis : null });
      }
      cam = to; t += dur;
    });
    for (const k of cy.keepAfter || []) {
      const id = bind(typeof k === 'string' ? k : k.ref, cy.structure, reg);
      kept.set(id, (typeof k === 'object' && k.style) || null);
    }
  });
  return { duration: t, steps, warnings, pivot, fovOf, bounds: b, aspect, target: ctx.target || null, scene: seq.scene || {}, srFps: (seq.data && seq.data.srFps) || 10,
    loop: (seq.output && seq.output.loop) || 'loop' };
}

// Zones -> intervals along the sweep axis for slab centre s: sweeping up (dir > 0) the wake is below.
function zoneIntervals(zones, sw, s) {
  if (!sw || zones.includes('all')) return null;
  const lo = s - sw.slab / 2, hi = s + sw.slab / 2;
  const Zs = sw.dir > 0 ? { behind: [-BIG, lo], at: [lo, hi], ahead: [hi, BIG] } : { behind: [hi, BIG], at: [lo, hi], ahead: [-BIG, lo] };
  const iv = zones.map(z => Zs[z]).filter(r => r && r[1] > r[0]).sort((a, b) => a[0] - b[0]);
  const out = [];
  for (const r of iv) { const l = out[out.length - 1]; if (l && r[0] <= l[1]) l[1] = Math.max(l[1], r[1]); else out.push(r.slice()); }
  return out;
}

// ---- evaluate ----
// opts.loop: 'once' (clamp), 'loop' or 'pingpong' (t wraps; pingpong plays the sequence forward then backward).
function evalCompiled(C, t, opts) {
  const D = C.duration, mode = (opts && opts.loop) || 'once';
  if (D <= 0) return null;
  if (mode === 'loop') t = ((t % D) + D) % D;
  else if (mode === 'pingpong') { const p = ((t % (2 * D)) + 2 * D) % (2 * D); t = p <= D ? p : 2 * D - p; }
  t = Math.min(D - 1e-9, Math.max(0, t));
  let lo = 0, hi = C.steps.length - 1;
  while (lo < hi) { const m = (lo + hi + 1) >> 1; if (C.steps[m].t0 <= t) lo = m; else hi = m - 1; }
  const S = C.steps[lo], dur = S.t1 - S.t0, u = (t - S.t0) / dur, ts = t - S.t0;
  const lerp = (a, b, f) => a + (b - a) * f;
  const az = lerp(S.camFrom.az, S.camTo.az, ease(S.ease.az, u));
  const tilt = lerp(S.camFrom.tilt, S.camTo.tilt, ease(S.ease.tilt, u));
  const fit = Math.exp(lerp(Math.log(S.camFrom.fit), Math.log(S.camTo.fit), ease(S.ease.fit, u)));
  const azRad = az * Math.PI / 180;
  // the sweep at this time
  let sweep = null, s = 0, axis = null;
  if (S.sweep) {
    s = lerp(S.sweep.from, S.sweep.to, ease(S.sweep.ease, u));
    let sAbs = s;
    if (S.sweep.relToCentre) { const a = axisVec(S.sweep.axis, azRad); sAbs += a[0] * C.bounds.center[0] + a[1] * C.bounds.center[1]; }
    axis = S.sweep.axis === 'h' || S.sweep.axis === 'd' ? axisVec(S.sweep.axis, azRad) : S.sweep.axis;
    sweep = { axis, pos: sAbs, slab: S.sweep.slab, dir: S.sweep.dir, u: ease(S.sweep.ease, u) };
    s = sAbs;
  }
  const fadeIn = S.fadeSec > 0 ? ease('smooth', ts / S.fadeSec) : 1;
  const layers = {};
  // one entry per layer id; a layer listed twice (e.g. bright in the slab, dim behind it) gets each listing's
  // intervals with that listing's style
  for (const l of S.layers) {
    if (l.followsPlane) {   // an image slice: drawn at the plane (its 'in slab' zone), whatever the slab's thickness
      if (S.sweep && (l.zones.includes('at') || l.zones.includes('all'))) {
        layers[l.id] = { intervals: null, style: l.style || undefined };
        if (S.entering.has(l.id) && fadeIn < 1) layers[l.id].alpha = fadeIn;
      }
      continue;
    }
    const iv = S.sweep ? zoneIntervals(l.zones, { slab: S.sweep.slab, dir: S.sweep.dir }, s) : null;
    if (iv && !iv.length) continue;
    const ivs = iv ? iv.map(([a, b2]) => ({ lo: a, hi: b2, style: l.style || undefined })) : null;
    const prev = layers[l.id];
    const entry = prev && prev.intervals && ivs ? { intervals: prev.intervals.concat(ivs).sort((x, y) => x.lo - y.lo) }
      : { intervals: ivs || [{ lo: -BIG, hi: BIG, style: l.style || undefined }] };
    if (S.entering.has(l.id) && fadeIn < 1) entry.alpha = fadeIn;
    if (l.plane || (prev && prev.plane)) entry.plane = l.plane || prev.plane;
    layers[l.id] = entry;
  }
  // the previous step's layers that this one drops fade out over fadeSec (frozen where they were)
  if (S.fadeSec > 0 && ts < S.fadeSec) {
    const a = 1 - ease('smooth', ts / S.fadeSec);
    for (const x of S.exiting) {
      if (layers[x.id]) continue;
      const sameAxis = !!sweep && x.axis === (S.sweep && S.sweep.axis);
      layers[x.id] = { style: x.style || undefined, intervals: sameAxis && x.intervals ? x.intervals.map(([lo2, hi2]) => ({ lo: lo2, hi: hi2 })) : null, alpha: a };
    }
  }
  const sc = C.scene || {};
  const capFade = 0.35, cap = S.caption ? Math.min(1, ts / capFade, (dur - ts) / capFade) : 0;
  return {
    t, step: lo, cycle: S.ci, u,
    camera: { pivot: C.pivot.slice(), azimuthDeg: az, tiltDeg: tilt, fovUm: C.fovOf(fit, tilt), fit },
    scope: sc.scope || 'ghosts', target: C.target || null, detailCells: sc.detailCells === undefined ? 1 : sc.detailCells,
    theme: sc.theme || 'fluo', ghostOpacity: sc.ghostOpacity, crop: sc.crop !== false,
    zClip: null, sweep: sweep ? { axis: sweep.axis } : null, slice: sweep ? { axis: sweep.axis, pos: sweep.pos, slab: sweep.slab, dir: sweep.dir } : null,
    srFrame: Math.floor(ts * C.srFps), layers,
    overlays: { caption: S.caption ? { text: S.caption, alpha: Math.max(0, cap) } : null,
      readout: sweep ? { axis: S.sweep.axis, pos: sweep.pos } : null },
  };
}

// ---- presets ----
// make(params) -> a cycle with '$.' refs (bound to params.structure). requires: the reps the structure must have.
const PRESETS = {
  orbit: { label: 'Orbit', requires: ['gt'], params: { duration: 8, deg: 360, tilt: 20 },
    make: p => ({ name: 'Orbit', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Orbit', duration: p.duration, camera: { tilt: { to: p.tilt } }, fadeSec: 0.4,
        layers: [{ ref: '$.gt', zones: ['all'] }, { ref: 'cyto.surface', zones: ['all'], style: { opacity: 0.15 } }] }] }) },
  popIn: { label: 'Pop in', requires: ['gt'], params: { duration: 3 },
    make: p => ({ name: 'Pop in', structure: p.structure, orbit: { deg: 0 }, steps: [
      { name: 'Pop in', duration: p.duration, fadeSec: 0.4, layers: [{ ref: '$.gt', zones: ['all'] }] }] }) },
  buildUp: { label: 'Build up (bottom to top)', requires: ['gt'], params: { duration: 6, deg: 90 },
    make: p => ({ name: 'Build up', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Bottom to top', duration: p.duration, sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' },
        layers: [{ ref: 'cyto.surface', zones: ['ahead'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] }] }) },
  movingSlab: { label: 'Moving slab', requires: ['gt'], params: { duration: 6, slab: 0.6, deg: 0 },
    make: p => ({ name: 'Moving slab', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Slab, top to bottom', duration: p.duration, sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: p.slab, ease: 'linear' },
        layers: [{ ref: 'cyto.surface', zones: ['all'], style: { opacity: 0.12 } }, { ref: '$.gt', zones: ['at'] }] }] }) },
  sideSweep: { label: 'Side to side', requires: ['gt'], params: { duration: 5, deg: 0 },
    make: p => ({ name: 'Side to side', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Left to right', duration: p.duration, sweep: { axis: 'h', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' },
        layers: [{ ref: 'cyto.surface', zones: ['all'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] }] }) },
  upAndDown: { label: 'Up, then down with a slab', requires: ['gt'], params: { up: 6, down: 6, slab: 0.4, deg: 360 },
    make: p => ({ name: 'Up and down', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Simulated, bottom to top', duration: p.up, sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' },
        layers: [{ ref: 'cyto.surface', zones: ['ahead'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] },
      { name: 'Slab back down', duration: p.down, sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: p.slab, ease: 'inOut' },
        layers: [{ ref: '$.gt', zones: ['ahead', 'at'] }, { ref: '$.gt', zones: ['behind'], style: { opacity: 0.25 } }] }] }) },
  sliceDown: { label: 'Simulated up, image slices down', requires: ['gt', 'wfSlice'], params: { up: 6, down: 8, deg: 360, slice: 'wfSlice' },
    make: p => ({ name: 'Simulated -> slices', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Simulated, bottom to top', duration: p.up, caption: 'Simulated structure', sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' },
        layers: [{ ref: 'cyto.surface', zones: ['ahead'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] },
      { name: 'Focal planes, top to bottom', duration: p.down, caption: p.slice === 'srFrames' ? 'SMLM camera frames' : 'WideField z-stack', sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: 0, ease: 'linear' },
        layers: [{ ref: '$.gt', zones: ['ahead'], style: { opacity: 0.35 } }, { ref: '$.' + p.slice, zones: ['at'] }, { ref: '$.gt', zones: ['behind'], style: { opacity: 0.12 } }] }] }) },
  simUpWfDown: { label: 'Simulated up, WideField down, thresholded wake', requires: ['gt', 'wfSlice', 'wfIso'], params: { up: 6, down: 6, slab: 0.3, deg: 360 },
    make: p => ({ name: 'Simulated -> WideField -> thresholded', structure: p.structure, orbit: { deg: p.deg }, keepAfter: ['$.wfIso'], steps: [
      { name: 'Simulated, bottom to top', duration: p.up, caption: 'Simulated structure', sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' },
        layers: [{ ref: 'cyto.surface', zones: ['ahead'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] },
      { name: 'WideField slices down', duration: p.down, caption: 'WideField z-stack, thresholded', sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: p.slab, ease: 'inOut' },
        layers: [{ ref: '$.gt', zones: ['ahead'] }, { ref: '$.wfSlice', zones: ['at'] }, { ref: '$.wfIso', zones: ['behind'] }] }] }) },
};
PRESETS.simUpSmlmDown = { label: 'Simulated up, SMLM down, localizations wake', requires: ['gt', 'srFrames', 'locs'], params: { up: 6, down: 8, deg: 360 },
  make: p => ({ name: 'Simulated -> SMLM -> localizations', structure: p.structure, orbit: { deg: p.deg }, keepAfter: ['$.locs'], steps: [
    { name: 'Simulated, bottom to top', duration: p.up, caption: 'Simulated structure', sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' },
      layers: [{ ref: 'cyto.surface', zones: ['ahead'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] },
    { name: 'SMLM frames down, localizations behind', duration: p.down, caption: 'SMLM acquisition, localized', sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: 0, ease: 'linear' },
      layers: [{ ref: '$.gt', zones: ['ahead'], style: { opacity: 0.35 } }, { ref: '$.srFrames', zones: ['at'] }, { ref: '$.locs', zones: ['behind'] }] }] }) };
// A new cycle from a preset (ids filled in).
function cycleFromPreset(name, params) {
  const P = PRESETS[name];
  const p = Object.assign({}, P.params, params || {});
  const c = P.make(p);
  c.id = newId('c'); c.enabled = true; c.keepAfter = c.keepAfter || []; c.dropKept = [];
  c.from = { preset: name, params: p };
  for (const s of c.steps) { s.id = newId('s'); s.layers = s.layers || []; }
  return c;
}
// Can a structure take a preset (it has every rep the preset needs)?
function presetFits(name, structure, registry) {
  const P = PRESETS[name];
  if (!registry) return true;
  return P.requires.every(rep => {
    const id = bind('$.' + rep, structure, registry), L = registry.layers.get(id);
    return L && L.implemented !== false;
  });
}

// The data a compiled sequence needs before it can play (phase 3+: z-stacks, events), per layer id.
// -> [{layer, needs}] per data layer the sequence shows
function requiredData(C, registry) {
  const need = new Map();
  for (const S of C.steps) for (const l of S.layers) {
    const L = registry && registry.layers.get(l.id);
    if (L && L.needs && L.needs !== 'dyes') need.set(l.id, L.needs);
  }
  return [...need].map(([layer, needs]) => ({ layer, needs }));
}
// Problems worth showing in the editor: [{level, msg}].
function validate(seq, registry) {
  const out = [];
  try {
    const C = compileSequence(seq, { registry, bounds: { x: [0, 1], y: [0, 1], z: [0, 1], center: [0.5, 0.5, 0.5], diam: 1 } });
    if (!C.steps.length) out.push({ level: 'warn', msg: 'no steps yet: add a cycle' });
    for (const w of C.warnings) if (!/no target cell/.test(w)) out.push({ level: 'warn', msg: w });
    for (const S of C.steps) if (S.sweep && !S.sweep.slab && S.layers.some(l => !l.followsPlane && l.zones.length === 1 && l.zones[0] === 'at'))
      out.push({ level: 'warn', msg: `cycle ${S.ci + 1} step ${S.si + 1}: a layer only in the slab, but the slab is 0 um thick` });
  } catch (e) { out.push({ level: 'error', msg: e.message }); }
  return out;
}

// The default sequence of a new animation: an orbit with a build-up of the microtubules, then the nucleus pops in.
function starter() {
  const s = defaults();
  s.name = 'Microtubules, then nucleus';
  s.cycles.push(cycleFromPreset('upAndDown', { structure: 'mt', up: 6, down: 6, slab: 0.4, deg: 180 }));
  const n = cycleFromPreset('popIn', { structure: 'nucleus', duration: 4 });
  n.orbit = { degPerSec: 15 }; n.steps[0].layers.push({ ref: 'mt.lines', zones: ['all'], style: { opacity: 0.6 } });
  s.cycles.push(n);
  return s;
}

// ---- more cycle presets ----
const L_ = (ref, zones, style) => (style ? { ref, zones, style } : { ref, zones });
const GHOST = zones => L_('cyto.surface', zones, { opacity: 0.15 });
const SW = (axis, from, to, slab, ease) => ({ axis, from: { rel: from }, to: { rel: to }, slab: slab || 0, ease: ease || 'inOut' });
Object.assign(PRESETS, {
  flyAround: { label: 'Fly around (top view to side view)', requires: ['gt'], params: { duration: 10, deg: 360 },
    make: p => ({ name: 'Fly around', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Down from above', duration: p.duration / 2, fadeSec: 0.4, camera: { tilt: { to: 70 }, fit: { to: 1 } },
        layers: [L_('$.gt', ['all']), GHOST(['all'])] },
      { name: 'Back up', duration: p.duration / 2, camera: { tilt: { to: 15 }, fit: { to: 1.2 } }, layers: [L_('$.gt', ['all']), GHOST(['all'])] }] }) },
  turntable: { label: 'Turntable (side view, slow turn)', requires: ['gt'], params: { duration: 12, deg: 360 },
    make: p => ({ name: 'Turntable', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Side view turn', duration: p.duration, fadeSec: 0.4, camera: { tilt: { to: 80, ease: 'out' } },
        layers: [L_('$.gt', ['all']), L_('nucleus.surface', ['all'], { opacity: 0.3 }), GHOST(['all'])] }] }) },
  zoomDive: { label: 'Zoom dive into the cell', requires: ['gt'], params: { duration: 8, deg: 90 },
    make: p => ({ name: 'Zoom dive', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Dive in', duration: p.duration * 0.6, camera: { fit: { to: 0.35 }, tilt: { to: 45 } }, layers: [L_('$.gt', ['all']), GHOST(['all'])] },
      { name: 'Pull out', duration: p.duration * 0.4, camera: { fit: { to: 1.15 }, tilt: { to: 20 } }, layers: [L_('$.gt', ['all']), GHOST(['all'])] }] }) },
  topDownReveal: { label: 'Top view build-up, then tilt', requires: ['gt'], params: { up: 5, tilt: 4, deg: 0 },
    make: p => ({ name: 'Top-down reveal', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Build up from above', duration: p.up, camera: { tilt: { to: 0 } }, sweep: SW('z', 0, 1, 0),
        layers: [GHOST(['ahead']), L_('$.gt', ['behind'])] },
      { name: 'Tilt to show the height', duration: p.tilt, camera: { tilt: { to: 65 }, az: { by: 60 } }, layers: [L_('$.gt', ['all']), GHOST(['all'])] }] }) },
  peelAway: { label: 'Peel away the cytoplasm (top down)', requires: ['gt'], params: { duration: 6, deg: 120 },
    make: p => ({ name: 'Peel away', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Cytoplasm off, top to bottom', duration: p.duration, caption: 'Inside the cell', sweep: SW('z', 1, 0, 0),
        layers: [L_('cyto.surface', ['ahead'], { opacity: 0.55 }), L_('$.gt', ['all']), L_('nucleus.surface', ['all'], { opacity: 0.35 })] }] }) },
  xRay: { label: 'X-ray (the cell fades away)', requires: ['gt'], params: { duration: 6, deg: 180 },
    make: p => ({ name: 'X-ray', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'The cell', duration: p.duration / 3, fadeSec: 0.4, layers: [L_('cyto.surface', ['all'], { opacity: 0.6 }), L_('nucleus.surface', ['all'], { opacity: 0.5 })] },
      { name: 'Seen through', duration: p.duration * 2 / 3, fadeSec: 1.2, layers: [L_('$.gt', ['all']), GHOST(['all'])] }] }) },
  slabTour: { label: 'Optical sections up and down', requires: ['gt'], params: { duration: 10, slab: 0.4, deg: 0 },
    make: p => ({ name: 'Optical sections', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Up', duration: p.duration / 2, caption: 'Optical sections', camera: { tilt: { to: 35 } }, sweep: SW('z', 0, 1, p.slab, 'linear'),
        layers: [L_('$.gt', ['at']), L_('$.gt', ['ahead', 'behind'], { opacity: 0.1 }), GHOST(['all'])] },
      { name: 'Down', duration: p.duration / 2, sweep: SW('z', 1, 0, p.slab, 'linear'),
        layers: [L_('$.gt', ['at']), L_('$.gt', ['ahead', 'behind'], { opacity: 0.1 }), GHOST(['all'])] }] }) },
  depthScan: { label: 'Depth scan (front to back slab)', requires: ['gt'], params: { duration: 7, slab: 1.5 },
    make: p => ({ name: 'Depth scan', structure: p.structure, orbit: { deg: 0 }, steps: [
      { name: 'Front to back', duration: p.duration, camera: { tilt: { to: 60 } }, sweep: SW('d', 1, 0, p.slab, 'linear'),
        layers: [L_('$.gt', ['at']), L_('$.gt', ['ahead', 'behind'], { opacity: 0.08 }), GHOST(['all'])] }] }) },
  wipeIn: { label: 'Wipe in, left to right, while turning', requires: ['gt'], params: { duration: 6, deg: 90 },
    make: p => ({ name: 'Wipe in', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Wipe', duration: p.duration, sweep: SW('x', 0, 1, 0.6),
        layers: [GHOST(['ahead', 'at']), L_('$.gt', ['at'], { color: '#ffffff' }), L_('$.gt', ['behind'])] }] }) },
  shellToCore: { label: 'Outside in: cytoplasm, nucleus, then the structure', requires: ['gt'], params: { duration: 9, deg: 270 },
    make: p => ({ name: 'Outside in', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Cytoplasm', duration: p.duration / 3, caption: 'Cytoplasm', fadeSec: 0.5, layers: [L_('cyto.surface', ['all'], { opacity: 0.5 }), L_('cyto.outline', ['all'])] },
      { name: 'Nucleus', duration: p.duration / 3, caption: 'Nucleus', fadeSec: 0.5, layers: [GHOST(['all']), L_('nucleus.surface', ['all'], { opacity: 0.6 })] },
      { name: 'Structure', duration: p.duration / 3, caption: 'Structure', fadeSec: 0.5, layers: [GHOST(['all']), L_('nucleus.surface', ['all'], { opacity: 0.2 }), L_('$.gt', ['all'])] }] }) },
  dyeLattice: { label: 'Simulated to dye sites (z sweep)', requires: ['gt', 'dyes'], params: { duration: 7, deg: 60 },
    make: p => ({ name: 'Simulated -> dye sites', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Dyes below the plane', duration: p.duration, caption: 'Labelled dye sites', camera: { fit: { to: 0.6 } }, sweep: SW('z', 0, 1, 0, 'linear'),
        layers: [L_('$.gt', ['ahead']), L_('$.dyes', ['behind'])] }] }) },
  wfDepthSlice: { label: 'WideField x-z slice through depth', requires: ['gt', 'wfSlice'], params: { duration: 8 },
    make: p => ({ name: 'WideField x-z slice', structure: p.structure, orbit: { deg: 0 }, steps: [
      { name: 'Slice front to back', duration: p.duration, caption: 'WideField, x-z', camera: { tilt: { to: 65 } }, sweep: SW('d', 1, 0, 0, 'linear'),
        layers: [L_('$.gt', ['behind'], { opacity: 0.3 }), L_('$.wfSlice', ['at'])] }] }) },
  wipeSimToIso: { label: 'Wipe: simulated vs thresholded WideField', requires: ['gt', 'wfIso'], params: { duration: 8, deg: 0 },
    make: p => ({ name: 'Simulated | thresholded', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Wipe across', duration: p.duration, caption: 'Ground truth | segmented WideField', camera: { tilt: { to: 40 } }, sweep: SW('h', 0, 1, 0, 'inOut'),
        layers: [L_('$.gt', ['ahead']), L_('$.wfIso', ['behind'])] }] }) },
  wipeSimToLocs: { label: 'Wipe: simulated vs localizations', requires: ['gt', 'locs'], params: { duration: 8, deg: 0 },
    make: p => ({ name: 'Simulated | localized', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Wipe across', duration: p.duration, caption: 'Ground truth | SMLM localizations', camera: { tilt: { to: 30 } }, sweep: SW('h', 0, 1, 0, 'inOut'),
        layers: [L_('$.gt', ['ahead']), L_('$.locs', ['behind'])] }] }) },
  smlmBuild: { label: 'SMLM reconstruction builds up, turning', requires: ['gt', 'locs'], params: { duration: 10, deg: 360 },
    make: p => ({ name: 'SMLM build-up', structure: p.structure, orbit: { deg: p.deg }, keepAfter: ['$.locs'], steps: [
      { name: 'Localizations bottom to top', duration: p.duration, caption: 'SMLM localizations', sweep: SW('z', 0, 1, 0, 'linear'),
        layers: [L_('$.gt', ['ahead'], { opacity: 0.25 }), L_('$.locs', ['behind'])] }] }) },
  smlmSlab: { label: 'SMLM frames in a slab, localizations around', requires: ['srFrames', 'locs'], params: { duration: 10, deg: 120 },
    make: p => ({ name: 'Blinking slab', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Slab down', duration: p.duration, caption: 'Blinking at the focus', sweep: SW('z', 1, 0, 0.4, 'linear'),
        layers: [L_('$.srFrames', ['at']), L_('$.locs', ['ahead', 'behind'], { opacity: 0.35 })] }] }) },
  bfFocus: { label: 'BrightField focus sweep', requires: [], params: { duration: 8, deg: 60 },
    make: p => ({ name: 'BrightField focus', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'Focus up through the cell', duration: p.duration, caption: 'BrightField, through focus', camera: { tilt: { to: 30 } }, sweep: SW('z', 0, 1, 0, 'linear'),
        layers: [L_('cell.bfSlice', ['at']), GHOST(['ahead'])] }] }) },
  allModalities: { label: 'Three modalities, one after another', requires: ['gt', 'wfSlice', 'srFrames'], params: { each: 5, deg: 360 },
    make: p => ({ name: 'Simulated, WideField, SMLM, BrightField', structure: p.structure, orbit: { deg: p.deg }, steps: [
      { name: 'WideField', duration: p.each, caption: 'WideField', sweep: SW('z', 1, 0, 0, 'linear'), layers: [L_('$.gt', ['ahead'], { opacity: 0.3 }), L_('$.wfSlice', ['at'])] },
      { name: 'SMLM', duration: p.each, caption: 'SMLM camera frames', sweep: SW('z', 0, 1, 0, 'linear'), layers: [L_('$.gt', ['ahead'], { opacity: 0.3 }), L_('$.srFrames', ['at'])] },
      { name: 'BrightField', duration: p.each, caption: 'BrightField', sweep: SW('z', 1, 0, 0, 'linear'), layers: [L_('$.gt', ['ahead'], { opacity: 0.3 }), L_('cell.bfSlice', ['at'])] }] }) },
});

// ---- whole sequences ("New from…") ----
function seqOf(name, cycles, cam) {
  const s = defaults();
  s.name = name;
  Object.assign(s.camera, cam || {});
  for (const [preset, params, extra] of cycles) { const c = cycleFromPreset(preset, params); Object.assign(c, extra || {}); s.cycles.push(c); }
  return s;
}
const SEQUENCES = {
  starter: { label: 'Microtubules, then nucleus', make: () => starter() },
  example: { label: 'Simulated, WideField, thresholded (needs data)', make: () => example() },
  grandTour: { label: 'Grand tour: outside in, fly around, sections', make: () => seqOf('Grand tour', [
    ['shellToCore', { structure: 'mt', duration: 9, deg: 180 }], ['flyAround', { structure: 'mt', duration: 8, deg: 180 }],
    ['slabTour', { structure: 'mt', duration: 8, slab: 0.4, deg: 90 }]]) },
  groundTruthVsData: { label: 'Ground truth vs data: wipes (needs data)', make: () => seqOf('Ground truth vs data', [
    ['wipeSimToIso', { structure: 'mt', duration: 7 }], ['wipeSimToLocs', { structure: 'mt', duration: 7 }]], { tilt: 35 }) },
  smlmStory: { label: 'An SMLM experiment (needs data)', make: () => seqOf('An SMLM experiment', [
    ['buildUp', { structure: 'mt', duration: 5, deg: 90 }], ['smlmSlab', { structure: 'mt', duration: 8, deg: 120 }],
    ['smlmBuild', { structure: 'mt', duration: 8, deg: 150 }]]) },
  modalities: { label: 'One cell, every modality (needs data)', make: () => seqOf('Every modality', [
    ['popIn', { structure: 'mt', duration: 3 }, { orbit: { degPerSec: 20 } }], ['allModalities', { structure: 'mt', each: 5, deg: 300 }]]) },
  smlmUpLocsDown: { label: 'SMLM slab up, localizations down (needs data)', make: () => smlmUpLocsDown() },
  turntableShow: { label: 'Turntable: microtubules and nucleus', make: () => seqOf('Turntable', [
    ['turntable', { structure: 'mt', duration: 12, deg: 360 }]], { tilt: 70 }) },
};

// The issue's example: the microtubules simulated bottom to top while the view turns, WideField slices back down
// leaving the thresholded surface, then the nucleus pops in with the surface kept.
function example() {
  const s = defaults();
  s.name = 'Simulated, WideField, thresholded; then the nucleus';
  s.camera.tilt = 20;
  s.cycles.push(cycleFromPreset('simUpWfDown', { structure: 'mt', up: 6, down: 8, slab: 0.3, deg: 360 }));
  const n = cycleFromPreset('popIn', { structure: 'nucleus', duration: 4 });
  n.orbit = { degPerSec: 25 };
  s.cycles.push(n);
  return s;
}

// The user's own animation (2026-10-05; iterate here): the simulated cell (microtubules, nucleus, cytoplasm) from
// above, an SMLM slab rising through it with the microtubules ahead while the view tilts and zooms in, a plane back
// down leaving the localizations, the localizations turned to the top view, then the nucleus and cytoplasm with them.
function smlmUpLocsDown() {
  return migrate({ format: FORMAT, version: 1, name: "SMLM slab up, localizations down",
    scene: { target: { mode: "center", cell: null }, scope: "target", ghostOpacity: 0.25, theme: "fluo", detailCells: 1, crop: true },
    camera: { az: 0, tilt: 65, fit: 1.15, center: { dx: 0, dy: 0, dz: 0 }, orbitDegPerSec: 0 },
    data: { srFps: 10, stepUm: 0.2, srStepUm: 0.4, srFrames: 10, wfAverage: 4, crop: false, sequential: false, locFrames: 5000 },
    output: { format: "mp4", width: 1920, height: 1080, fps: 30, quality: "high", loop: "loop", overlays: { scaleBar: true, captions: true, readout: true, legend: false }, gif: { width: 480, fps: 20, dither: "bayer4" } },
    cycles: [{ name: "Simulated -> SMLM -> localizations", structure: "mt", enabled: true, orbit: { deg: 360 }, keepAfter: ["$.locs"], dropKept: [], steps: [
      { name: "Hold", duration: 2, layers: [{ zones: ["all"], ref: "$.gt", style: { color: "#ff5151", opacity: 0.5 } }, { zones: ["all"], ref: "nucleus.surface", style: { color: "#000080" } }, { zones: ["all"], ref: "cyto.surface" }], camera: { tilt: { to: 0, ease: "inOut" } }, caption: "Simulation" },
      { name: "SMLM frames up, cytoplasm ahead", duration: 8, layers: [{ zones: ["all"], ref: "cyto.surface", style: { color: "#264678" } }, { zones: ["at"], ref: "$.srFrames", style: { color: "#ffffff" } }, { zones: ["ahead"], ref: "$.gt", style: { opacity: 0.5, color: "#ff5151" } }], caption: "Simulated SMLM data", sweep: { axis: "z", from: { rel: 0, um: 0 }, to: { rel: 1, um: 0 }, slab: 0.4, ease: "linear" }, camera: { tilt: { to: 70, ease: "inOut" }, fit: { to: 0.35, ease: "inOut" } }, fadeSec: 0 },
      { name: "Top to bottom, localizations behind", duration: 6, layers: [{ zones: ["behind"], ref: "$.locs", style: { color: "#ffc850", opacity: 0.2 } }, { zones: ["at"], ref: "$.srFrames", style: { color: "#ffffff", opacity: 1 } }], sweep: { axis: "z", from: { rel: 1, um: 0 }, to: { rel: 0, um: 0 }, slab: 0, ease: "inOut" }, camera: { fit: { to: 0.85, ease: "inOut" } }, fadeSec: 0, caption: "Localized SMLM data" },
      { name: "Hold", duration: 4, layers: [{ zones: ["all"], ref: "$.locs", style: { opacity: 0.2 } }], camera: { tilt: { to: 0, ease: "inOut" }, az: { by: 120, ease: "inOut" } }, caption: "SMLM localizations" },
      { name: "Hold", duration: 2, layers: [{ zones: ["all"], ref: "nucleus.surface", style: { opacity: 1, color: "#000080" } }, { zones: ["all"], ref: "$.locs", style: { opacity: 0.05 } }, { zones: ["all"], ref: "cyto.surface" }], camera: { tilt: { to: 80, ease: "inOut" } }, fadeSec: 0, caption: "SMLM localizations" },
      { name: "Hold", duration: 2, layers: [{ ref: "$.locs", zones: ["all"], style: { opacity: 0.05 } }, { ref: "cyto.surface", zones: ["all"] }, { ref: "nucleus.surface", zones: ["all"], style: { color: "#000080", opacity: 1 } }], camera: { az: { by: 360, ease: "inOut" }, tilt: { to: 0, ease: "inOut" } }, fadeSec: 0, caption: "SMLM localizations" }] }] }).seq;
}

globalThis.IscAnimSeq = { FORMAT, VERSION, EASE, ease, defaults, migrate, bind, axisVec, extentAlong, compileSequence, zoneIntervals,
  evalCompiled, PRESETS, cycleFromPreset, presetFits, requiredData, validate, starter, example, SEQUENCES, newId, clone };
})();
