// Compares parity outputs against the JS reference and prints the M0 table.
//   node compare.mjs <ref_js.txt> <name>=<out.txt> [<name>=<out.txt> ...] [--tol 1e-6]
// Exit code 0 = PASS (RNG bit-exact, every layout equal or within tolerance).
import fs from 'fs';

const args = process.argv.slice(2);
let tol = 1e-6;
const ti = args.indexOf('--tol');
if (ti >= 0) { tol = +args[ti + 1]; args.splice(ti, 2); }
const [refPath, ...targets] = args;

function parse(path) {
  const r = { rng: [], stream: [], math: [], cases: new Map() };
  let cur = null;
  for (const line of fs.readFileSync(path, 'utf8').split(/\r?\n/)) {
    if (!line) continue;
    const t = line.split(' ');
    switch (t[0]) {
      case 'rng': r.rng.push(t.slice(1)); break;
      case 'stream': r.stream.push(t.slice(1)); break;
      case 'm': r.math.push(t.slice(1)); break;
      case 'case':
        cur = { id: t[1], ncand: +t[3], removed: +t[5], ms: +t[7], raw: new Map(), pk: new Map() };
        r.cases.set(cur.id, cur); break;
      case 'raw': cur.raw.set(t[1] + ',' + t[2], t.slice(3).map(Number)); break;
      case 'pk': cur.pk.set(t[1] + ',' + t[2], t.slice(3).map(Number)); break;
    }
  }
  return r;
}

const ref = parse(refPath);
const outs = targets.map(s => { const [name, p] = s.split('='); return { name, r: parse(p) }; });

// Numbers: parsed back to doubles, so "0.1" and "0.10000000000000001" compare equal.
function cmpNumLists(a, b) {
  let maxd = 0, exact = a.length === b.length;
  for (let i = 0; i < Math.min(a.length, b.length); i++) {
    const x = Number(a[i]), y = Number(b[i]);
    if (Object.is(x, y) || (Number.isNaN(x) && Number.isNaN(y))) continue;
    exact = false;
    maxd = Math.max(maxd, Math.abs(x - y));
  }
  return { exact, maxd };
}

let pass = true;
const lines = [];
const say = s => { lines.push(s); console.log(s); };

// ---- RNG / math ----
say('## Primitives\n');
say('| check | ' + outs.map(o => o.name).join(' | ') + ' |');
say('|---|' + outs.map(() => '---|').join(''));
const rngRow = outs.map(o => {
  let bad = 0;
  ref.rng.forEach((a, i) => { const c = cmpNumLists(a, o.r.rng[i] || []); if (!c.exact) bad++; });
  ref.stream.forEach((a, i) => { const c = cmpNumLists(a, o.r.stream[i] || []); if (!c.exact) bad++; });
  if (bad || o.r.rng.length !== ref.rng.length) pass = false;
  return bad ? `**${bad} mismatches**` : `bit-exact (${ref.rng.length} addr + ${ref.stream.length} streams)`;
});
say('| pcg4d / hashUnit / hashStream | ' + rngRow.join(' | ') + ' |');
const FN = ['sin', 'cos', 'atan2', 'hypot', 'atan', 'exp', 'log'];
FN.forEach((fn, k) => {
  const row = outs.map(o => {
    let bad = 0;
    ref.math.forEach((a, i) => { if (!o.r.math[i] || o.r.math[i][k + 1] !== a[k + 1]) bad++; });
    return bad ? `${bad} / ${ref.math.length} differ` : `bit-exact (${ref.math.length})`;
  });
  say(`| Math.${fn} vs core jsm::${fn} | ` + row.join(' | ') + ' |');
});

// ---- cases ----
say('\n## Packing, per case\n');
say('`equal` = every double bit-identical; `near` = same surviving cells, max |Δ| ≤ ' + tol +
    ' µm/rad; `DIVERGES` = different cells survive or |Δ| > tol.\n');
say('| case | cand | removed (js) | ' + outs.map(o => o.name + ' raw | ' + o.name + ' packed').join(' | ') + ' | ms js | ' + outs.map(o => 'ms ' + o.name).join(' | ') + ' |');
say('|---|---|---|' + outs.map(() => '---|---|').join('') + '---|' + outs.map(() => '---|').join(''));
const tally = Object.fromEntries(outs.map(o => [o.name, { equal: 0, near: 0, diverges: 0 }]));
for (const [id, rc] of ref.cases) {
  const cells = [];
  const ms = [];
  for (const o of outs) {
    const oc = o.r.cases.get(id);
    if (!oc) { cells.push('missing', 'missing'); pass = false; tally[o.name].diverges++; continue; }
    // raw candidates
    let rawExact = rc.raw.size === oc.raw.size, rawMax = 0;
    for (const [k, v] of rc.raw) {
      const w = oc.raw.get(k);
      if (!w) { rawExact = false; rawMax = Infinity; continue; }
      const c = cmpNumLists(v, w); rawExact &&= c.exact; rawMax = Math.max(rawMax, c.maxd);
    }
    // packed
    let sameSet = rc.pk.size === oc.pk.size, pkExact = true, pkMax = 0;
    for (const [k, v] of rc.pk) {
      const w = oc.pk.get(k);
      if (!w) { sameSet = false; continue; }
      const c = cmpNumLists(v, w); pkExact &&= c.exact; pkMax = Math.max(pkMax, c.maxd);
    }
    const fmt = (exact, same, maxd) => !same ? 'DIVERGES (cell set)' : exact ? 'equal' : maxd <= tol ? `near ${maxd.toExponential(1)}` : `DIVERGES ${maxd.toExponential(1)}`;
    const rawS = fmt(rawExact, rawMax !== Infinity, rawMax);
    const pkS = fmt(sameSet && pkExact, sameSet, pkMax);
    cells.push(rawS, pkS);
    const verdict = pkS.startsWith('DIVERGES') || rawS.startsWith('DIVERGES') ? 'diverges' : pkS === 'equal' && rawS === 'equal' ? 'equal' : 'near';
    tally[o.name][verdict]++;
    if (verdict === 'diverges') pass = false;
    ms.push(oc.ms.toFixed(1));
  }
  say(`| ${id} | ${rc.ncand} | ${rc.removed} | ${cells.join(' | ')} | ${rc.ms.toFixed(1)} | ${ms.join(' | ')} |`);
}
say('\n## Summary\n');
for (const o of outs) {
  const t = tally[o.name];
  say(`- **${o.name}**: ${t.equal} equal, ${t.near} near, ${t.diverges} diverging (of ${ref.cases.size})`);
}
say(`\n**${pass ? 'PASS' : 'FAIL'}**`);
process.exit(pass ? 0 : 1);
