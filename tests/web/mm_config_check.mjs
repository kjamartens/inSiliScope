// The live panel's Configuration (web/live/mm_config_ui.js) against what it reads: every preset of every group of the
// shipped Micro-Manager configurations (web/live/mm_config.js, written by tools/gen_mm_configs.py) must map onto the
// viewer -- a settings control mv_<option> that can show the value (a select's choice, a number within the control's
// limits) or an option the viewer's WASM module knows (sent as is) -- and the laser sliders' rows must be controls.
// Needs no build or browser (the committed module and page).   node tests/web/mm_config_check.mjs
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const t0 = performance.now();
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8').replace(/\r\n/g, '\n');
const self = {};
new Function('self', read('web/live/mm_config.js'))(self);
new Function('self', read('web/dye_library.js'))(self);
const CFG = self.ISC_MM_CONFIG, DD = self.ISC_DYE_DATA;

// The viewer's settings controls (UI_GROUPS of web/index.html), by id.
const html = read('web/index.html'), open = '<script id="uiSchema">', s = html.indexOf(open);
const { UI_GROUPS } = new Function('self', html.slice(s + open.length, html.indexOf('</script>', s)) +
  '\nreturn { UI_GROUPS };')({ ISC_DYE_DATA: DD });
const controls = new Map();
const walk = e => { if (e.id) controls.set(e.id, e); for (const k of e.kids || []) walk(k); };
for (const g of UI_GROUPS) g.items.forEach(walk);

// The module's movie options and their defaults (isc_scope_options).
const { M } = await loadWasmScope(read('web/insiliscope_module.js'));
const n = M._isc_scope_options(0, 0) + 1, b = M._malloc(n);
M._isc_scope_options(b, n);
const engine = new Map(M.UTF8ToString(b).split('\n').filter(Boolean).map(l => l.split('\t')).map(l => [l[0], +l[1]]));
M._free(b);

const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
// = mm_config_ui.js FOLLOW: -1 = the camera preset's; the adapter's resolved value must be the library camera's
const FOLLOW = {
  'camera-type': c => (c.type === 'EMCCD' ? 1 : 0),
  'em-gain': c => (c.preampElectronsPerAdu || 1) / c.gainElectronsPerAdu,
  'qe-curve': c => DD.cameras.indexOf(c),
};
let fails = 0, checked = 0;
const fail = msg => { console.log('FAIL ' + msg); fails++; };
// = mm_config_ui.js valueOf, with the mode from the preset or the control's default
function valueOf(key, set) {
  let v = set[key];
  if (v === null) v = engine.has(key) ? engine.get(key) : NaN;
  const m = /^([a-z]+)-dye$/.exec(key);
  if (m && v === -1) {
    const t = DD.specimens.flatMap(sp => sp.targets).find(t => t.prefix === m[1]);
    const mk = m[1] + '-mode';
    const mode = MODES[set[mk] !== undefined && set[mk] !== null ? set[mk] : +controls.get('mv_' + mk).value];
    const typ = t && DD.typicalLabels[t.id] && DD.typicalLabels[t.id][mode];
    const i = typ ? DD.dyes.findIndex(d => d.id === typ.dye) : -1;
    if (i < 0) return NaN;
    if (!(DD.dyes[i].modes || {})[mode] && DD.dyes[i].modes) fail(`${key}: typical ${typ.dye} has no ${mode} data`);
    return i;
  }
  return v;
}
for (const [tier, T] of Object.entries(CFG)) {
  for (const g of T.groups) {
    if (g.slider) {
      const c = controls.get('mv_' + g.slider.key);
      checked++;
      if (!c) fail(`${tier} ${g.name}: no control mv_${g.slider.key}`);
      else if (c.max !== null && c.max !== undefined && c.max < g.slider.hi)
        console.log(`note ${tier} ${g.name}: the viewer's limit ${c.max} < Micro-Manager's ${g.slider.hi}`);
      continue;
    }
    if (!g.presets.length) fail(`${tier} ${g.name}: no presets`);
    for (const p of g.presets) {
      for (const k of Object.keys(p.set)) {
        checked++;
        const c = controls.get('mv_' + k), v = valueOf(k, p.set), at = `${tier} ${g.name} "${p.name}" ${k}=${p.set[k]}`;
        const cam = DD.cameras[p.set['camera-preset']];
        if (!Number.isFinite(v)) fail(`${at}: no value`);
        else if (FOLLOW[k] && cam) {
          if (Math.abs(FOLLOW[k](cam) - v) > 1e-9 * Math.abs(v)) fail(`${at}: not ${cam.id}'s ${FOLLOW[k](cam)}`);
          if (c && !c.options.some(o => +o[0] === -1)) fail(`mv_${k}: no -1 (the preset's) choice`);
        }
        else if (c && c.kind === 'select') {
          if (!c.options.some(o => String(o[0]) === String(v))) fail(`${at}: not a choice of mv_${k}`);
        } else if (c) {
          if ((c.min !== null && c.min !== undefined && v < c.min) || (c.max !== null && c.max !== undefined && v > c.max))
            fail(`${at}: outside mv_${k}'s ${c.min}..${c.max}`);
        } else if (!engine.has(k)) fail(`${at}: neither a viewer control nor a module option`);
      }
      if (p.light && !['epi', 'trans', 'both'].includes(p.light)) fail(`${tier} ${g.name} "${p.name}": light ${p.light}`);
    }
  }
  // the pixel size (mm_config_ui.js: sensor pixel / (objective x emission magnification)) within mv_pixel-nm's limits
  for (const g of T.groups) for (const p of g.presets || []) if (p.optics) {
    const o = { ...T.optics, ...p.optics }, px = o.sensorUm * 1000 / (o.objectiveMag * o.emissionMag), c = controls.get('mv_pixel-nm');
    checked++;
    if (!(px > 0) || !c || px < c.min || (c.max !== null && px > c.max)) fail(`${tier} ${g.name} "${p.name}": pixel ${px} nm`);
  }
  for (const [g, name] of Object.entries(T.startup))
    if (!T.groups.some(x => x.name === g && (x.slider || x.presets.some(p => p.name === name)))) fail(`${tier} startup ${g}=${name}`);
}
// The settings levels (index.html tierOf): every option's tier is one of MM's, and the label rows have theirs.
const TI = self.ISC_MM_TIERS;
if (!TI || !TI.options || !TI.props) fail('ISC_MM_TIERS missing');
else {
  for (const [k, v] of Object.entries(TI.options)) if (!['Basic', 'Advanced', 'Expert'].includes(v.tier) || !TI.props[v.prop]) fail(`tier of ${k}: ${JSON.stringify(v)}`);
  for (const k of ['mt-dye', 'mt-mode', 'laser-640', 'camera-preset']) if (!TI.options[k] || TI.options[k].tier !== 'Basic') fail(`${k} not Basic: ${JSON.stringify(TI.options[k])}`);
  checked += Object.keys(TI.options).length;
}
// The dye indices of the adapter's spec and the viewer's select are one list (library, Dye1..3, None).
const dyeSel = controls.get('mv_mt-dye');
if (!dyeSel || dyeSel.options.length !== DD.dyes.length + 4) fail('mv_mt-dye: not the library dyes + Dye1..3 + None');
console.log(`${fails ? 'FAIL' : 'ok'}: ${checked} preset options of ${Object.keys(CFG).join('/')} map onto the viewer` +
  ` (${((performance.now() - t0) / 1000).toFixed(1)} s)`);
process.exit(fails ? 1 : 0);
