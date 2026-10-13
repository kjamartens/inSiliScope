// The nanoruler model's checks (Node, seconds): tools/origami_reference/test_origami_bend.py's checks on the JS
// reference (web/prototype/scope/origami.js), plus the field (Matern thinning, determinism under any query order) and
// the switches (strands per spot, incorporation), and a cross-check against origami_bend.py fed the same normals.
//   node web/lab/origami_check.mjs [--rulers 4000] [--no-python]
// Exits 1 on a failed check. BSD-3-Clause.
import { execFileSync } from 'child_process';
import path from 'path';
import { fileURLToPath } from 'url';
import { STRUCTURE_ORIGAMI, ORIGAMI_DESIGNS, ORIGAMI_TILE_UM, OrigamiField, bentShape, centerline, transform, moleculeSites,
  normalizeOrigamiParams, bendNormal, moleculeH1, origamiGeometry, shapeOutline } from '../prototype/scope/origami.js';
import { World, STRUCTURES } from '../prototype/scope/world.js';
import { makeLabel } from '../prototype/scope/dyes.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const M = +opt('--rulers', 4000);
const t0 = performance.now();
let ok = true;
const check = (name, cond, detail = '') => { ok = ok && !!cond; console.log(`[${cond ? 'PASS' : 'FAIL'}] ${name} ${detail}`); };
const P0 = normalizeOrigamiParams({ specimen: 1 });
const d = ORIGAMI_DESIGNS[0], G0 = origamiGeometry(d, P0), L = G0.L, D = G0.H, R = D / 2;
const POS = d.spots.map(s => s.u * G0.s + L / 2);
const Pw = o => normalizeOrigamiParams({ specimen: 1, ...o });
const mol = (h1, extra = {}) => ({ tx: 0, ty: 0, i: 0, x: 0, y: 0, yaw: 0, h1, ...extra });
const maxAbs = (a, b) => a.reduce((m, v, i) => Math.max(m, Math.abs(v - b[i])), 0);
const wlc2dRms = (s, Lp) => Math.sqrt(4 * Lp * s * (1 - (2 * Lp / s) * (1 - Math.exp(-s / (2 * Lp)))));

// ---- geometry ----
const pts = [];
for (let j = 0; j < 500; j++) pts.push([(j * 0.6180339887 % 1 - 0.5) * L, (j * 0.7548776662 % 1 - 0.5) * D]);
const flat = S => pts.flatMap(([x, y]) => transform(S, x, y));
check('same molecule -> same shape', maxAbs(flat(bentShape(d, P0, 7)), flat(bentShape(d, P0, 7))) === 0);
check('different molecule -> different shape', maxAbs(flat(bentShape(d, P0, 7)), flat(bentShape(d, P0, 8))) > 1e-3);
{
  const S = bentShape(d, Pw({ origamiBend: 0 }), 1), e = maxAbs(flat(S), pts.flat());
  check('rigid rod maps to itself', e < 1e-9, `(max error ${e.toExponential(1)} nm)`);
  const S2 = bentShape(d, Pw({ origamiLpNm: 1e12 }), 1), e2 = maxAbs(flat(S2), pts.flat());
  check('Lp -> infinity maps to itself', e2 < 1e-3, `(max error ${e2.toExponential(1)} nm)`);
}
{
  const S = bentShape(d, Pw({ origamiLpNm: 273.1 }), 3);
  let len = 0, [px, py] = centerline(S, -L / 2);
  for (let j = 1; j <= 20000; j++) { const [x, y] = centerline(S, -L / 2 + L * j / 20000); len += Math.hypot(x - px, y - py); px = x; py = y; }
  check('centreline length preserved', Math.abs(len - L) < 1e-3, `(${len.toFixed(4)} nm)`);
}
{
  // Sites on the surface (linker 0, 3D heights): distance to the centreline point at their x is the radius.
  const P = Pw({ origamiLpNm: 273.1, origamiLinkerNm: 0, origamiHeights: 1, origamiAttachNm: 0 });
  const S = bentShape(d, P, 3), m = mol(3);
  const sites = moleculeSites(d, P, m, S, 1, 1, [], true);
  let lo = Infinity, hi = 0;
  for (const s of sites) {
    const [cx, cy] = centerline(S, s.rest[0]);
    const r = Math.hypot(Math.hypot(s.bent[0] - cx, s.bent[1] - cy), s.z * 1000 - R);
    lo = Math.min(lo, r); hi = Math.max(hi, r);
  }
  check(`sites stay on the rod surface (r = ${R} nm)`, Math.abs(lo - R) < 1e-9 && Math.abs(hi - R) < 1e-9, `(${lo.toFixed(6)}-${hi.toFixed(6)} nm)`);
  let e = 0;
  for (let k = 0; k < d.spots.length; k++) {
    const g = sites.filter(s => s.k === k), cx = g.reduce((a, s) => a + s.bent[0], 0) / g.length, cy = g.reduce((a, s) => a + s.bent[1], 0) / g.length;
    const [ax, ay] = centerline(S, POS[k] - L / 2);
    e = Math.max(e, Math.hypot(cx - ax, cy - ay));
  }
  check('each 5-site ring is centred on the axis', e < 1e-9, `(${e.toExponential(1)} nm)`);
  check('15 designed strands', sites.length === 15);
}
{
  // 2D (default): every site at the axis height above the glass.
  const P = Pw({}), S = bentShape(d, P, 5);
  const zs = moleculeSites(d, P, mol(5), S, 1, 1, [], true).map(s => s.z);
  check('2D: every strand at attach + D/2', zs.every(z => z === (P.origamiAttachNm + R) / 1000));
}

// ---- the site spacing ----
{
  check('80 nm spacing: the 200 nm rod with spots at 20, 100, 180 nm', L === 200 && D === 13 && POS.join() === '20,100,180');
  const rest = sp => moleculeSites(d, Pw({ origamiSpacingNm: sp, origamiBend: 0, origamiLinkerNm: 0 }), mol(5),
    bentShape(d, Pw({ origamiSpacingNm: sp, origamiBend: 0 }), 5), 1, 1, [], true);
  for (const sp of [640, 40, 5]) {
    const G = origamiGeometry(d, Pw({ origamiSpacingNm: sp })), xs = [0, 1, 2].map(k => rest(sp).find(s => s.k === k).rest[0]);
    check(`${sp} nm spacing: spots ${sp} nm apart, 20 nm ends`, G.L === 2 * sp + 40 &&
      xs.every((x, k) => Math.abs(x - (k - 1) * sp) < 1e-9), `(length ${G.L} nm, spots at ${xs.map(x => (x + G.L / 2).toFixed(1)).join(', ')})`);
  }
  // A different spacing never moves a molecule (placement draws do not depend on it).
  const key = m => `${m.tx},${m.ty},${m.i},${m.x},${m.y},${m.yaw}`;
  const tiles = F => [0, 1, 2].flatMap(t => F.tileMolecules(t, -t)).map(key).join(';');
  const A = tiles(new OrigamiField(9, Pw({}))), B = tiles(new OrigamiField(9, Pw({ origamiSpacingNm: 20 })));
  check('spacing leaves placement alone', A === B && A.length > 0);
}

// ---- the 2D origami (the TU Delft logo: a rigid hex-map plate, face up) ----
{
  const dl = ORIGAMI_DESIGNS.findIndex(x => x.id === 'TUDelftLogo'), D2 = ORIGAMI_DESIGNS[dl];
  check('the logo design exists', dl > 0 && D2.shape === 'plate' && D2.spots.length === 37 && D2.maxRingN === 1);
  for (const sp of [80, 20, 5]) {
    const P = Pw({ origamiDesign: dl, origamiSpacingNm: sp, origamiLinkerNm: 0 });
    check(`logo: strands per spot clamps to 1`, P.origamiStrandsPerSpot === 1);
    const G = origamiGeometry(D2, P), S = bentShape(D2, P, 77);
    const sites = moleculeSites(D2, P, mol(77, { yaw: 0.7 }), S, 1, 1, [], true);
    // Nearest-neighbour centre distance = the spacing; every strand on the plate's top face, inside it.
    let nn = Infinity, inside = true;
    for (let a = 0; a < sites.length; a++) {
      const [x, y] = sites[a].rest;
      inside = inside && Math.abs(x) < G.L / 2 && Math.abs(y) < G.W / 2;
      for (let b = a + 1; b < sites.length; b++) nn = Math.min(nn, Math.hypot(x - sites[b].rest[0], y - sites[b].rest[1]));
    }
    check(`logo at ${sp} nm: 37 strands, neighbours ${sp} nm apart, inside the plate`, sites.length === 37 &&
      Math.abs(nn - sp) < 1e-9 * sp && inside, `(nearest ${nn.toFixed(6)} nm, plate ${G.L.toFixed(1)} x ${G.W.toFixed(1)} nm)`);
    check(`logo at ${sp} nm: rigid and on the top face`, sites.every(s => Math.abs(s.bent[0] - s.rest[0]) < 1e-9 &&
      Math.abs(s.bent[1] - s.rest[1]) < 1e-9 && s.rest[2] === G.H / 2));
  }
  // Face up, never mirrored: the flame (cell 0601, column 6 row 1) lies up-left of the o (1506) in the molecule frame
  // (+y = down, as on the screen), whatever the roll params say.
  const P = Pw({ origamiDesign: dl, origamiRoll: 1, origamiRollDeg: 180 });
  const sites = moleculeSites(D2, P, mol(3), bentShape(D2, P, 3), 1, 1, [], true);
  const at = id => sites[D2.spots.findIndex(s => s.id === id)].rest;
  const cross = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  check('logo lies face up (unmirrored, roll ignored)', at('0601')[0] < at('1506')[0] && at('0601')[1] < at('1506')[1] &&
    Math.sign(cross(at('0601'), at('1506'), at('0809'))) === Math.sign(cross([6, 1.0], [15, 6.5], [8, 9.0])));
  const o = shapeOutline(bentShape(D2, P, 3));
  const G = origamiGeometry(D2, P);
  check('logo outline = the plate rectangle', o.every((v, j) => Math.abs(Math.abs(v) - (j % 2 ? G.W : G.L) / 2) < 1e-9 ||
    Math.abs(v) < (j % 2 ? G.W : G.L) / 2 + 1e-9));
}

// ---- the normals ----
{
  let s1 = 0, s2 = 0, n = 0;
  for (let h = 1; h <= 2000; h++) for (let i = 0; i < 31; i++) { const z = bendNormal(h * 2654435761 >>> 0, i); s1 += z; s2 += z * z; n++; }
  const mean = s1 / n, v = s2 / n - mean * mean;
  check('bending normals ~ N(0, 1)', Math.abs(mean) < 0.02 && Math.abs(v - 1) < 0.02, `(mean ${mean.toFixed(4)}, var ${v.toFixed(4)}, ${n} draws)`);
}

// ---- statistics (test_origami_bend.py), at the default Lp and at the explorer's 273.1 nm ----
function stats(Lp) {
  const P = Pw({ origamiLpNm: Lp, origamiLinkerNm: 0 });
  const shapes = [];
  for (let j = 0; j < M; j++) shapes.push(bentShape(d, P, moleculeH1(42, j, 0, 0)));
  const ends = shapes.map(S => S.endAngle);
  const rmsEnd = Math.sqrt(ends.reduce((a, e) => a + e * e, 0) / M), mEnd = ends.reduce((a, e) => a + e, 0) / M;
  const sdEnd = Math.sqrt(rmsEnd * rmsEnd - mEnd * mEnd), pred = Math.sqrt(L / Lp);
  check(`Lp ${Lp}: end-angle RMS = sqrt(L/Lp)`, Math.abs(sdEnd / pred - 1) < 0.04,
    `(measured ${(sdEnd * 180 / Math.PI).toFixed(1)} deg, predicted ${(pred * 180 / Math.PI).toFixed(1)} deg)`);
  let dev = 0;
  for (let j = 0; j <= 8; j++) {
    const s = L * j / 8;
    const c = shapes.reduce((a, S) => a + Math.cos(centerline(S, -L / 2 + s)[2] - centerline(S, -L / 2)[2]), 0) / M;
    dev = Math.max(dev, Math.abs(c - Math.exp(-s / (2 * Lp))));
  }
  check(`Lp ${Lp}: tangent correlation = exp(-s/2Lp)`, dev < 0.02, `(max deviation ${dev.toFixed(4)})`);
  const pairs = [[0, 1], [1, 2], [0, 2]];
  const dist = pairs.map(() => []);
  shapes.forEach((S, j) => {
    const sites = moleculeSites(d, Pw({ origamiLpNm: Lp, origamiLinkerNm: 0 }), mol(moleculeH1(42, j, 0, 0)), S, 1, 1, [], true);
    const c = [0, 1, 2].map(k => { const g = sites.filter(s => s.k === k); return [g.reduce((a, s) => a + s.bent[0], 0) / g.length, g.reduce((a, s) => a + s.bent[1], 0) / g.length]; });
    pairs.forEach(([a, b], pi) => dist[pi].push(Math.hypot(c[a][0] - c[b][0], c[a][1] - c[b][1])));
  });
  pairs.forEach(([a, b], pi) => {
    const v = dist[pi], s = POS[b] - POS[a];
    const mean = v.reduce((x, y) => x + y, 0) / M, rms = Math.sqrt(v.reduce((x, y) => x + y * y, 0) / M), th = wlc2dRms(s, Lp);
    check(`Lp ${Lp}: ${POS[a]}-${POS[b]} nm RMS matches the 2D WLC`, Math.abs(rms / th - 1) < 0.005,
      `(straight ${s}, mean ${mean.toFixed(2)}, RMS ${rms.toFixed(2)}, WLC ${th.toFixed(2)})`);
  });
  const folds = shapes.filter(S => S.fold).length;
  console.log(`[INFO] Lp ${Lp}: ${folds} of ${M} molecules with a bend radius below the half-width`);
}
stats(P0.origamiLpNm);
stats(273.1);

// ---- the field ----
{
  const seed = 1234, P = Pw({ origamiDensity: 2, origamiMinSpacingNm: 250 });
  const F = new OrigamiField(seed, P), S = 20;
  const mols = F.moleculesInRect(0, 0, S, S, -F.reachUm()).filter(m => m.x >= 0 && m.x < S && m.y >= 0 && m.y < S);
  let dmin = Infinity;
  const all = F.moleculesInRect(-1, -1, S + 1, S + 1, -F.reachUm());
  for (let a = 0; a < all.length; a++) for (let b = a + 1; b < all.length; b++)
    dmin = Math.min(dmin, Math.hypot(all[a].x - all[b].x, all[a].y - all[b].y));
  check('minimum spacing holds', dmin >= 0.25, `(closest centres ${(dmin * 1000).toFixed(1)} nm)`);
  const lam = 2, r = 0.25, lamII = (1 - Math.exp(-lam * Math.PI * r * r)) / (Math.PI * r * r);
  const got = mols.length / (S * S);
  check('Matern-II density', Math.abs(got / lamII - 1) < 0.05, `(${got.toFixed(3)} /um^2, theory ${lamII.toFixed(3)})`);
  // Query-order independence: the same rect from a fresh field after other queries, and tile by tile.
  const G = new OrigamiField(seed, P);
  G.moleculesInRect(37, -12, 45, -3); G.moleculesInRect(5.5, 5.5, 6, 6);
  const key = m => `${m.tx},${m.ty},${m.i},${m.x},${m.y},${m.yaw}`;
  const a1 = F.moleculesInRect(3, 3, 9, 9).map(key).join(';'), a2 = G.moleculesInRect(3, 3, 9, 9).map(key).join(';');
  check('molecules independent of query history', a1 === a2 && a1.length > 0);
}
{
  // A World of the Nanorulers specimen: sites the same from any query order, labels select subsets.
  const params = { specimen: 1 }, lab = STRUCTURES.map((s, i) => makeLabel(i === STRUCTURE_ORIGAMI ? { density: 1 } : {}));
  const W1 = new World({ gen: {} }, 77, params, lab), W2 = new World({ gen: {} }, 77, params, lab);
  W2.sitesInWindow(-30, -30, -20, -20, -1, 1);
  const s1 = JSON.stringify(W1.sitesInWindow(0, 0, 8, 8, -1, 1)), s2 = JSON.stringify(W2.sitesInWindow(0, 0, 8, 8, -1, 1));
  check('world sites independent of query history', s1 === s2 && s1.length > 2);
  const all = W1.sitesInWindow(0, 0, 30, 30, -1, 1);
  check(`world sites are structure ${STRUCTURE_ORIGAMI} only`, all.every(s => s.structure === STRUCTURE_ORIGAMI) && all.length > 0, `(${all.length} sites)`);
  const W3 = new World({ gen: {} }, 77, { specimen: 1, origamiStrandsPerSpot: 3 }, lab);
  const sub = W3.sitesInWindow(0, 0, 30, 30, -1, 1);
  const k5 = new Set(all.map(s => `${s.cx},${s.cy},${s.mtIndex},${s.k},${s.n},${s.x},${s.y}`));
  check('strands per spot 3 = ring positions 0-2 of the same strands', sub.length > 0 && sub.every(s => s.n < 3 && k5.has(`${s.cx},${s.cy},${s.mtIndex},${s.k},${s.n},${s.x},${s.y}`)),
    `(${sub.length} of ${all.length})`);
  const W4 = new World({ gen: {} }, 77, params, STRUCTURES.map((s, i) => makeLabel(i === STRUCTURE_ORIGAMI ? { density: 0.8 } : {})));
  const inc = W4.sitesInWindow(0, 0, 30, 30, -1, 1);
  check('incorporation 80 % keeps a subset', inc.every(s => k5.has(`${s.cx},${s.cy},${s.mtIndex},${s.k},${s.n},${s.x},${s.y}`)) && Math.abs(inc.length / all.length - 0.8) < 0.03,
    `(${(100 * inc.length / all.length).toFixed(1)} %)`);
  check('no cells in a nanoruler world', W1.cellsInRect(0, 0, 30, 30).length === 0);
}

// ---- cross-check against origami_bend.py ----
if (!args.includes('--no-python')) {
  const cases = [];
  for (const [h, Lp] of [[11, 273.1], [12, 1500], [13, 100]]) {
    const n = Math.max(8, Math.ceil(L / d.segmentNm));
    const normals = Array.from({ length: n }, (_, i) => bendNormal(h, i));
    cases.push({ h, Lp, normals, roll: 0.3 + h / 10, lengthNm: L, diameterNm: D, lpNm: Lp, segmentNm: d.segmentNm,
      positionsNm: POS, nAround: 5 });
  }
  let res = null;
  try {
    res = JSON.parse(execFileSync('python', ['-I', path.join(ROOT, 'tools/origami_reference/crosscheck.py')],
      { input: JSON.stringify({ cases }), encoding: 'utf8' })).results;
  } catch (e) { console.log(`[SKIP] cross-check with origami_bend.py (python with numpy needed: ${e.message.split('\n')[0]})`); }
  if (res) cases.forEach((c, ci) => {
    const P = Pw({ origamiLpNm: c.Lp, origamiLinkerNm: 0, origamiHeights: 1, origamiAttachNm: 0, origamiRoll: 1, origamiRollDeg: c.roll * 180 / Math.PI });
    const S = bentShape(d, P, c.h, c.normals);
    const js = moleculeSites(d, P, mol(c.h), S, 1, 1, [], true);
    let e = 0;
    js.forEach((s, j) => { const q = res[ci].sites[s.k * 5 + s.n]; e = Math.max(e, Math.abs(s.bent[0] - q[0]), Math.abs(s.bent[1] - q[1]), Math.abs(s.z * 1000 - q[2])); });
    check(`origami_bend.py: same sites, Lp ${c.Lp}`, e < 1e-9 && js.length === 15 && Math.abs(S.endAngle - res[ci].endAngle) < 1e-12 && S.n === res[ci].n,
      `(max difference ${e.toExponential(1)} nm)`);
  });
}

console.log(`origami check: ${ok ? 'PASS' : 'FAIL'} (${((performance.now() - t0) / 1000).toFixed(1)} s)`);
process.exit(ok ? 0 : 1);
