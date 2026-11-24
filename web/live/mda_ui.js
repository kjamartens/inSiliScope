// SPDX-License-Identifier: BSD-3-Clause
// The multi-D plan's editor (2026-11-23; the Record box's Edit…, Advanced and Expert): the positions, then the plan as
// nested rows of for blocks (over positions, Z heights, any setting) and recordings, run top to bottom, inside out.
// Under them: what is wrong, or the totals and the first blocks in the order they run. Edits a copy; Done keeps it
// (web/live/live.js L.recPlan.set), Cancel or Esc drops it. The model is web/live/mda.js.
'use strict';
(function () {
const D = () => self.ISC_MDA, LP = () => self.liveState && self.liveState.recPlan;
const esc = s => String(s === undefined || s === null ? '' : s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
const css = document.createElement('style');
css.textContent = `
  #setDlg .box.mda{width:min(820px,calc(100vw - 32px))}
  #setDlg .mdaSec{margin:var(--space-3) 0 var(--space-1);font-weight:600}
  #setDlg .mdaPos,#setDlg .mdaNode{display:flex;flex-wrap:wrap;align-items:center;gap:var(--space-1) var(--space-2);margin:var(--space-1) 0;font-size:var(--font-size-caption)}
  #setDlg .mdaNode{border-left:2px solid var(--color-border);padding:var(--space-1) 0 var(--space-1) var(--space-2)}
  #setDlg .mdaNode.rec{border-left-color:var(--color-accent)}
  #setDlg .mdaNode .k{font-weight:600;color:var(--color-text)}
  #setDlg .mdaNode .u,#setDlg .mdaPos .u{color:var(--color-text-muted)}
  #setDlg .mdaNode input[type=text]{flex:1 1 8em;min-width:6em}
  #setDlg .mdaNode textarea{flex:1 1 12em;min-width:8em;box-sizing:border-box;resize:none;overflow:hidden;font:var(--font-size-caption) var(--font-family-mono)}
  #setDlg .mdaNode textarea.tall{flex-basis:100%;order:1}
  #setDlg .mdaNode input.key{flex:0 1 12em}
  #setDlg .mdaNode input.nm{flex:0 1 6em}
  #setDlg .mdaPos input[type=text]{flex:0 1 6em} #setDlg .mdaPos input[type=number]{width:6em}
  #setDlg .mdaNode button,#setDlg .mdaPos button{padding:1px 6px}
  #setDlg .mdaNode .sp{flex:1}
  #setDlg .mdaKids{margin-left:var(--space-4)}
  #setDlg .mdaCheck{margin:var(--space-2) 0;font-size:var(--font-size-caption);max-height:200px;overflow:auto;white-space:pre-wrap}
  #setDlg .mdaCheck .bad{color:var(--color-status-error)}
  #setDlg .mdaCheck .ok{color:var(--color-text-muted)}
  .mdaPick{position:fixed;z-index:var(--z-tooltip);max-height:240px;overflow:auto;background:var(--color-surface-raised);color:var(--color-text);border:1px solid var(--color-border-strong);border-radius:var(--radius-md);box-shadow:var(--shadow-raised);font-size:var(--font-size-caption)}
  .mdaPick .it{padding:var(--space-1) var(--space-2);cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .mdaPick .it span{color:var(--color-text-muted)}
  .mdaPick .it.on{background:var(--color-surface-hover)}`;
document.head.append(css);

// The settings a for block or a recording can set, by the name each one's help shows (its Micro-Manager property:
// live.js planKeys, web/live/mm_cfg.js; an engine key where there is none): the settings column's rows with their
// label, unit, group and help (their own tip, shown as in the column), then the properties and engine options no row
// shows; not those a plan may not set (mda.js refusedWhy). name -> { name, label, tip, choices, whole, text (search) }
const BOX_NAMES = { simPhases: 'Lasers.PatternPhaseMatrix', simBeams: 'Lasers.Beams', zernCoeffs: 'Objective.ZernikeCoefficients' };
const K = () => (LP() && LP().keys && LP().keys()) || null;
function catalog() {
  const out = new Map(), k = K(), eng = (LP() && LP().eng()) || new Map();
  const nameOf = o => (k ? k.nameOf(o) : o);
  const add = (n, e) => { if (!n || out.has(n) || D().refusedWhy(n)) return; out.set(n, { name: n, ...e, text: ((e.label || '') + ' ' + (e.tip || '')).replace(/<[^>]+>/g, ' ').toLowerCase() }); };
  try {
    // (UI_GROUPS: the settings' rows, web/index.html; a row's kids are rows too)
    const walk = (it, g) => {
      if (!it) return;
      const id = it.id || '', o = /^mv_/.test(id) ? id.slice(3) : null, n = o ? nameOf(o) : BOX_NAMES[id];
      if (n && !(o && D().refusedWhy(o))) add(n, { label: `${it.label || n}${it.unit ? ' (' + it.unit + ')' : ''}`, group: g.title || '', tip: it.tip || '', el: id });
      (it.kids || []).forEach(x => walk(x, g));
    };
    for (const g of UI_GROUPS) (g.items || []).forEach(it => walk(it, g));
  } catch (e) { /* the rows not reachable: the names alone */ }
  const all = new Map(k ? k.names().map(e => [e.name, e]) : []);
  for (const [nm, e] of all) if (!(e.option && D().refusedWhy(e.option))) add(nm, { label: '', group: '', tip: '' });
  if (!k) for (const [o] of eng) if (!D().refusedWhy(o)) add(o, { label: '', group: '', tip: '' });
  for (const e of out.values()) {
    const ks = all.get(e.name), en = eng.get(e.name);
    e.choices = (ks && ks.choices) || (en && en.names) || null;
    e.whole = !!(k ? k.whole(e.name) : en && en.text);
  }
  return out;
}
// a name a recording's settings text may start a setting with (the catalog's, or a key the spec takes: zern.<j>, ...)
const isKey = n => !!(cat && cat.has(n)) || !!(LP() && LP().known(n) && LP().eng() && (LP().eng().has(n) || /\./.test(n)));
const whole = n => !!(cat && cat.get(n) && cat.get(n).whole) || !!(K() && K().whole(n));
let plan = null, box = null, cat = null;
const kidsOf = (parent) => parent ? (parent.children || (parent.children = [])) : plan.root;
const newRecord = () => ({ kind: 'record', name: 'R' + (countRecords(plan.root) + 1), frames: 10, exposureMs: 50, settings: {} });
const newLoop = () => ({ kind: 'loop', axis: 'z', values: '0', relative: true, children: [] });
const countRecords = nodes => (nodes || []).reduce((s, n) => s + (n.kind === 'record' ? 1 : countRecords(n.children)), 0);
// a copy of a block and what is inside it, its recordings renamed (A -> A2, R1 -> R2: the next name not taken)
function copyOf(n) {
  const c = JSON.parse(JSON.stringify(n)), names = new Set();
  const all = nodes => (nodes || []).forEach(x => { if (x.kind === 'record') names.add(x.name); all(x.children); });
  all(plan.root);
  const rename = x => {
    if (x.kind === 'record') {
      const stem = String(x.name || 'R').replace(/\d+$/, '');
      let k = 2;
      while (names.has(stem + k)) k++;
      x.name = stem + k;
      names.add(x.name);
    }
    (x.children || []).forEach(rename);
  };
  rename(c);
  return c;
}
// a for block's values field says what its setting takes: the choices, or for a matrix how several are written
function valuesHint(row, name) {
  const v = row.querySelector('[data-f=values]'), e = cat && cat.get(name);
  if (!v || !name) return;
  v.placeholder = e && e.choices ? e.choices.join(', ') : e && e.whole ? 'as in its box (rows by ;); several: each in "…", by commas' : '1, 2, 3 or start:step:stop';
}
// A value as the editor shows it: a matrix of numbers (a whole setting's rows) a row per line, after `indent`, without
// quotes (a whole value runs on to the next name=); anything else as mda.js quotes it.
const NUM_ROW = /^[-+\d.eE\s,]+$/;
function showValue(n, v, indent) {
  if (whole(n)) {
    const rows = String(v).split(/[;\n]/).map(r => r.trim().replace(/\s+/g, ' ')).filter(Boolean);
    if (rows.length > 1 && rows.every(r => NUM_ROW.test(r))) return rows.join('\n' + indent);
  }
  return D().quote(v);
}
// a for block's values as text: a list's matrices each in quotes, one after the other
function valuesText(n) {
  if (!Array.isArray(n.values)) return n.values;
  if (!(n.axis === 'setting' && whole(n.key))) return n.values.map(D().quote).join(', ');
  if (n.values.length === 1) return showValue(n.key, n.values[0], '');
  return n.values.map(v => '"' + String(showValue(n.key, v, '')).replace(/^"|"$/g, '') + '"').join(',\n');
}
// a recording's own settings as text, a setting per line (Lasers.Pattern=SIM2D, then Lasers.PatternPhaseMatrix=0 0
// and its other rows on lines of their own); and back
const settingsText = o => Object.entries(o || {}).map(([k, v]) => k + '=' + showValue(k, v, '  ')).join('\n');
const settingsOf = text => D().parseSettings(text, isKey, whole);
// a text box as high as its text, on a line of its own once it holds more than a short line (a matrix, several settings)
function grow(el) {
  if (el.tagName !== 'TEXTAREA') return;
  el.classList.toggle('tall', el.value.includes('\n') || el.value.length > 48);   // (a line of its own under the row)
  el.style.height = 'auto';
  el.style.height = el.scrollHeight + (el.offsetHeight - el.clientHeight) + 'px';
}

// A setting's value now (what picking it fills in): the box's own text for a matrix the settings have not typed over
// (rows as '; '), else the live spec's value by the setting's name.
function currentValue(n) {
  const e = cat && cat.get(n);
  let v;
  try { v = K() && self.iscSettingsCore ? K().current(n, self.iscSettingsCore.parseSpec(movieSpec())) : undefined; } catch (x) { v = undefined; }
  if ((v === undefined || v === '') && e && e.el) {
    const el = document.getElementById(e.el);
    if (el && el.value !== undefined && el.value.trim() !== '')
      v = e.whole ? el.value.split('\n').map(r => r.trim().replace(/\s+/g, ' ')).filter(Boolean).join('; ') : el.value;
  }
  return v;
}

// The help of a setting, as the settings column shows it (its row's own tip; a name with no row: the name alone).
const helpOf = e => (e.label ? `<b>${esc(e.label)}</b>${e.group ? ' · ' + esc(e.group) : ''}<br>` : '') + (e.tip || `<code>${esc(e.name)}</code>`);
const TIP = () => self.iscTip;

// The suggestion list under a setting field (in place of the browser's datalist, which shows no help): the settings
// whose name, label or help match what is typed; the one under the pointer or the arrow keys shows its help in the
// settings' tooltip. mode 'key': the field is one name (a for block's); 'settings': the name at the caret in
// "name=value; name=value" (a recording's). A pick writes the name and the setting's value now.
const pick = document.createElement('div');
pick.className = 'mdaPick';
pick.setAttribute('role', 'listbox');
pick.hidden = true;
let pk = null;   // { input, mode, items, active, from, to }
function queryAt(input, mode) {
  const v = input.value;
  if (mode === 'key') return { q: v.trim(), from: 0, to: v.length };
  const c = input.selectionStart === null ? v.length : input.selectionStart;
  let a = Math.max(v.lastIndexOf(';', c - 1), v.lastIndexOf(',', c - 1), v.lastIndexOf('\n', c - 1)) + 1;
  if (v.slice(a, c).includes('=') || (v.slice(0, a).split('"').length % 2 === 0)) return null;   // (in a value)
  while (v[a] === ' ') a++;
  let b = c;
  while (b < v.length && !/[=;,\n]/.test(v[b])) b++;
  const q = v.slice(a, c).trim();
  if (q && !/^[A-Za-z]/.test(q)) return null;   // (a matrix's next row: names start with a letter)
  return { q, from: a, to: b };
}
function matches(q) {
  const t = q.toLowerCase(), out = [];
  for (const e of cat.values()) {
    const n = e.name.toLowerCase(), l = (e.label || '').toLowerCase();
    const s = !t ? 4 : n.startsWith(t) || n.split('.').pop().startsWith(t) ? 0 : l.startsWith(t) ? 1 : n.includes(t) || l.includes(t) ? 2 : e.text.includes(t) ? 3 : -1;
    if (s >= 0) out.push([s, e]);
  }
  out.sort((a, b) => a[0] - b[0] || a[1].name.localeCompare(b[1].name));
  return out.slice(0, 50).map(x => x[1]);
}
function closePick() { pick.hidden = true; pk = null; if (TIP()) TIP().hide(); }
function showPick(input, mode, all) {
  const at = queryAt(input, mode);
  if (!at || (!at.q && !all)) { closePick(); return; }
  const items = matches(at.q);
  if (!items.length) { closePick(); return; }
  pk = { input, mode, items, active: -1, from: at.from, to: at.to };
  pick.innerHTML = items.map((e, i) => `<div class="it" role="option" data-i="${i}"><b>${esc(e.name)}</b>` +
    (e.label ? ` <span>${esc(e.label)}${e.group ? ' · ' + esc(e.group) : ''}</span>` : '') + '</div>').join('');
  const r = input.getBoundingClientRect();
  const w = Math.min(Math.max(r.width, 320), innerWidth - 16);
  Object.assign(pick.style, { left: Math.max(8, Math.min(r.left, innerWidth - 8 - w)) + 'px', top: r.bottom + 2 + 'px', width: w + 'px' });
  pick.hidden = false;
  if (pick.getBoundingClientRect().bottom > innerHeight - 8) pick.style.top = Math.max(8, r.top - 2 - pick.offsetHeight) + 'px';
  setActive(0);
}
function setActive(i) {
  if (!pk) return;
  pk.active = Math.max(0, Math.min(pk.items.length - 1, i));
  pick.querySelectorAll('.it').forEach((el, j) => el.classList.toggle('on', j === pk.active));
  const el = pick.children[pk.active];
  if (el) el.scrollIntoView({ block: 'nearest' });
  pick.dataset.tip = helpOf(pk.items[pk.active]);
  if (!TIP()) return;
  TIP().show(pick);
  // the help beside the list, level with the entry (below or above it would cover the field); where neither side has
  // room, where the tooltip put it
  const t = document.getElementById('tip'), pr = pick.getBoundingClientRect(), tr = t.getBoundingClientRect(), ir = el.getBoundingClientRect();
  let x = pr.left - tr.width - 6;
  if (x < 8) x = pr.right + 6;
  if (x + tr.width > innerWidth - 8) return;
  t.style.left = x + 'px';
  t.style.top = Math.max(8, Math.min(ir.top, innerHeight - 8 - tr.height)) + 'px';
}
function choose(i) {
  if (!pk) return;
  const e = pk.items[i], { input, mode, from, to } = pk, v = input.value, cur = currentValue(e.name);
  const val = cur === undefined ? '' : mode === 'settings' ? showValue(e.name, cur, '  ') : String(cur);
  closePick();
  if (mode === 'key') {
    input.value = e.name;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    const vals = input.closest('.mdaNode').querySelector('[data-f=values]');   // (empty values: the setting's value now)
    if (vals && !vals.value.trim() && val !== '') {
      vals.value = showValue(e.name, cur, '');
      vals.dispatchEvent(new Event('input', { bubbles: true }));
      grow(vals);
    }
  } else {
    const rest = v.slice(to), hasEq = /^\s*=/.test(rest);
    const head = v.slice(0, from) + e.name + '=' + (hasEq ? '' : val);
    input.value = head + (hasEq ? rest.replace(/^\s*=/, '') : rest);
    input.setSelectionRange(head.length, head.length);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    grow(input);
  }
  input.focus();
}
pick.addEventListener('mousedown', ev => { ev.preventDefault(); const it = ev.target.closest('.it'); if (it) choose(+it.dataset.i); });
pick.addEventListener('pointerover', ev => { const it = ev.target.closest('.it'); if (it && pk && +it.dataset.i !== pk.active) setActive(+it.dataset.i); });
// a setting field: suggestions as it is typed, the help of what is in it on hover; a one-line field takes a pasted
// matrix's lines as '; ' (a text box keeps them)
function settingField(input, mode) {
  input.setAttribute('autocomplete', 'off');
  input.setAttribute('data-now', '');   // (web/live/settle.js leaves its keystrokes alone: the list follows each one)
  const multi = input.tagName === 'TEXTAREA';
  input.addEventListener('input', ev => { if (ev.isTrusted) showPick(input, mode, false); });
  input.addEventListener('keydown', ev => {
    if (pk && pk.input === input) {
      if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') { ev.preventDefault(); setActive(pk.active + (ev.key === 'ArrowDown' ? 1 : -1)); return; }
      if (ev.key === 'Enter' || ev.key === 'Tab') { ev.preventDefault(); choose(pk.active); return; }
      if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); closePick(); return; }
    } else if ((ev.key === 'ArrowDown' && (!multi || input.value.indexOf('\n', input.selectionStart) < 0)) || (ev.key === ' ' && ev.ctrlKey)) {
      ev.preventDefault();   // (↓ on a text box's last line, or Ctrl+Space: every setting)
      showPick(input, mode, true);
    }
  });
  input.addEventListener('blur', () => setTimeout(() => { if (pk && pk.input === input) closePick(); }, 0));
  input.addEventListener('paste', ev => {
    const t = ev.clipboardData && ev.clipboardData.getData('text');
    if (multi || !t || !/\n/.test(t)) return;
    ev.preventDefault();
    input.setRangeText(t.split(/\r?\n/).map(r => r.trim().replace(/\s+/g, ' ')).filter(Boolean).join('; '), input.selectionStart, input.selectionEnd, 'end');
    input.dispatchEvent(new Event('input', { bubbles: true }));
  });
  input.addEventListener('pointerenter', () => {
    if (pk || !TIP()) return;
    const names = mode === 'key' ? [input.value.trim()] : Object.keys(settingsOf(input.value));
    const es = names.map(n => cat.get(n)).filter(Boolean);
    if (!es.length) return;
    input.dataset.tip = es.map(helpOf).join('<hr>');
    TIP().show(input);
  });
  input.addEventListener('pointerleave', () => { if (!pk && TIP()) TIP().hide(); });
}

function render() {
  const q = s => box.querySelector(s);
  const base = LP().base(), f = v => D().fmt(+(+v).toFixed(3));
  box.innerHTML = '<h3>Multi-D plan</h3>' +
    '<div class="msg">Nested for blocks, run top to bottom: each for block runs what is inside it once per value. A recording takes its frames at ' +
    'its exposure, with its own settings (name=value, one per line) on top of the for blocks\' values; ⧉ copies a block to just below it. One continuing live sample; saved as one stack per ' +
    'position and channel (Save: OME-TIFF, ImageJ or flat, and index.csv).</div>' +
    '<div class="mdaSec">Positions</div><div id="mdaPos"></div>' +
    `<div class="row" style="margin-top:0"><button id="mdaPosAdd" title="Add the stage's x, y and the focus now (${f(base.x)}, ${f(base.y)}, ${f(base.z)} µm)">+ current stage</button></div>` +
    '<div class="mdaSec">Blocks</div><div id="mdaTree"></div>' +
    '<div class="row" style="margin-top:0"><button id="mdaAddLoop">+ for block</button><button id="mdaAddRec">+ recording</button></div>' +
    '<div class="mdaCheck" id="mdaCheck" role="status"></div>' +
    '<div class="row"><button id="mdaExample" title="Replace the plan by a small example">Example</button><span class="sp"></span><button id="mdaCancel">Cancel</button><button id="mdaDone">Done</button></div>';
  // positions
  const pos = q('#mdaPos');
  plan.positions.forEach((p, i) => {
    const row = document.createElement('div');
    row.className = 'mdaPos';
    row.innerHTML = `<input type="text" aria-label="Position ${i + 1} name" value="${esc(p.name)}">` +
      `<span class="u">x</span><input type="number" step="0.1" aria-label="x, µm" value="${esc(p.x)}">` +
      `<span class="u">y</span><input type="number" step="0.1" aria-label="y, µm" value="${esc(p.y)}">` +
      `<span class="u">z</span><input type="number" step="0.1" aria-label="z, µm (empty: the focus at Start)" placeholder="focus" value="${p.z === null || p.z === undefined ? '' : esc(p.z)}"><span class="u">µm</span>` +
      '<button title="Move the stage and focus there">go to</button><button title="Remove this position" aria-label="Remove">✕</button>';
    const [nm, x, y, z] = row.querySelectorAll('input'), [go, rm] = row.querySelectorAll('button');
    nm.oninput = () => { p.name = nm.value.trim(); check(); };
    x.oninput = () => { p.x = x.value === '' ? '' : +x.value; check(); };
    y.oninput = () => { p.y = y.value === '' ? '' : +y.value; check(); };
    z.oninput = () => { p.z = z.value === '' ? null : +z.value; check(); };
    go.onclick = () => LP().goTo(+p.x, +p.y, p.z === null || p.z === undefined || p.z === '' ? NaN : +p.z);
    rm.onclick = () => { plan.positions.splice(i, 1); render(); };
    pos.append(row);
  });
  q('#mdaPosAdd').onclick = () => {
    const b = LP().base(), used = new Set(plan.positions.map(p => p.name));
    let k = plan.positions.length + 1;
    while (used.has('P' + k)) k++;
    plan.positions.push({ name: 'P' + k, x: +(+b.x).toFixed(3), y: +(+b.y).toFixed(3), z: +(+b.z).toFixed(3) });
    render();
  };
  // the blocks
  const tree = q('#mdaTree');
  const draw = (nodes, host, parent) => nodes.forEach((n, i) => {
    const row = document.createElement('div');
    row.className = 'mdaNode' + (n.kind === 'record' ? ' rec' : '');
    const tools = '<span class="sp"></span>' + (n.kind === 'loop' ? '<button data-a="loop" title="A for block inside this one">+ for</button><button data-a="rec" title="A recording inside this for block">+ record</button>' : '') +
      '<button data-a="dup" title="Copy this block (with what is inside) to just below it" aria-label="Copy below">⧉</button>' +
      '<button data-a="up" title="Move up" aria-label="Move up">↑</button><button data-a="down" title="Move down" aria-label="Move down">↓</button>' +
      '<button data-a="rm" title="Remove (with what is inside)" aria-label="Remove">✕</button>';
    if (n.kind === 'record') {
      row.innerHTML = '<span class="k">Record</span>' +
        `<input type="text" class="nm" data-f="name" aria-label="Recording name" value="${esc(n.name)}">` +
        `<input type="number" data-f="frames" min="1" step="1" aria-label="Frames" value="${esc(n.frames)}"><span class="u">frames at</span>` +
        `<input type="number" data-f="exposureMs" min="0.1" step="1" aria-label="Exposure, ms" value="${esc(n.exposureMs)}"><span class="u">ms</span>` +
        `<textarea rows="1" data-f="settings" spellcheck="false" aria-label="This recording's own settings, name=value, one per line" placeholder="settings: name=value, one per line (type a name, ↓: all)">${esc(settingsText(n.settings))}</textarea>` + tools;
    } else {
      const vals = valuesText(n);
      row.innerHTML = '<span class="k">For</span>' +
        '<select data-f="axis" aria-label="For what"><option value="position">position</option><option value="z">Z height</option><option value="setting">setting</option></select>' +
        (n.axis === 'setting' ? `<input type="text" class="key" data-f="key" aria-label="Setting" placeholder="setting (type, ↓: all)" value="${esc(n.key || '')}">` : '') +
        '<span class="u">in</span>' +
        `<textarea rows="1" data-f="values" spellcheck="false" aria-label="Values" placeholder="${n.axis === 'position' ? 'P1, P2' : n.axis === 'z' ? '0, 0.5, 1 or -1:0.25:1 (µm)' : '1, 2, 3 or start:step:stop'}">${esc(vals)}</textarea>` +
        (n.axis === 'z' ? `<label class="u" title="On: offsets from the position's z (the focus at Start without a position); off: heights above the coverslip"><input type="checkbox" data-f="relative"${n.relative ? ' checked' : ''}> relative</label>` : '') + tools;
      const ax = row.querySelector('[data-f=axis]');
      ax.value = n.axis;
      ax.onchange = () => {
        n.axis = ax.value;
        n.values = n.axis === 'position' ? plan.positions.map(p => p.name).join(', ') : n.axis === 'z' ? '0' : '';
        if (n.axis !== 'setting') delete n.key;
        render();
      };
    }
    for (const el of row.querySelectorAll('[data-f]')) {
      if (el.dataset.f === 'axis') continue;
      el.addEventListener(el.type === 'checkbox' ? 'change' : 'input', () => {
        const k = el.dataset.f;
        n[k] = el.type === 'checkbox' ? el.checked : k === 'settings' ? settingsOf(el.value) : k === 'frames' || k === 'exposureMs' ? +el.value : el.value.trim();
        if (k === 'key') valuesHint(row, n.key);
        grow(el);
        check();
      });
    }
    const keyEl = row.querySelector('[data-f=key]'), setEl = row.querySelector('[data-f=settings]');
    if (keyEl) { settingField(keyEl, 'key'); valuesHint(row, n.key); }
    if (setEl) settingField(setEl, 'settings');
    const sib = kidsOf(parent);
    for (const b of row.querySelectorAll('button[data-a]')) b.onclick = () => {
      const a = b.dataset.a;
      if (a === 'loop') kidsOf(n).push(newLoop());
      else if (a === 'rec') kidsOf(n).push(newRecord());
      else if (a === 'rm') sib.splice(i, 1);
      else if (a === 'dup') sib.splice(i + 1, 0, copyOf(n));
      else { const j = a === 'up' ? i - 1 : i + 1; if (j < 0 || j >= sib.length) return; [sib[i], sib[j]] = [sib[j], sib[i]]; }
      render();
    };
    host.append(row);
    if (n.kind === 'loop') { const kids = document.createElement('div'); kids.className = 'mdaKids'; host.append(kids); draw(n.children || [], kids, n); }
  });
  draw(plan.root, tree, null);
  for (const t of tree.querySelectorAll('textarea')) grow(t);
  q('#mdaAddLoop').onclick = () => { plan.root.push(newLoop()); render(); };
  q('#mdaAddRec').onclick = () => { plan.root.push(newRecord()); render(); };
  q('#mdaExample').onclick = () => { plan = exampleNow(); render(); };
  q('#mdaCancel').onclick = () => self.iscSettings.closeDialog();
  q('#mdaDone').onclick = () => { LP().set(plan); self.iscSettings.closeDialog(); };
  check();
}
// a small case of the kind asked for: two positions (here and 2 µm along x), two heights, recording A at two powers of
// the first laser, then recording B beside that for block
function exampleNow() {
  const b = LP().base(), r = v => +(+v).toFixed(3), eng = LP().eng();
  const laser = eng && [...eng.keys()].find(k => /^laser-\d+$/.test(k));
  const A = { kind: 'record', name: 'A', frames: 10, exposureMs: 50, settings: {} };
  return { positions: [{ name: 'P1', x: r(b.x), y: r(b.y), z: r(b.z) }, { name: 'P2', x: r(b.x + 2), y: r(b.y), z: r(b.z) }], root: [
    { kind: 'loop', axis: 'position', values: 'P1, P2', children: [
      { kind: 'loop', axis: 'z', values: '0, 0.5', relative: true, children: [
        laser ? { kind: 'loop', axis: 'setting', key: laser, values: '0.5, 1', children: [A] } : A,
        { kind: 'record', name: 'B', frames: 5, exposureMs: 100, settings: {} }] }] }] };
}
function check() {
  const el = box.querySelector('#mdaCheck'), c = LP().check(plan), side = LP().side();
  if (c.errs.length) {
    el.innerHTML = c.errs.slice(0, 12).map(e => `<div class="bad">${esc(e)}</div>`).join('') + (c.errs.length > 12 ? `<div class="bad">… ${c.errs.length - 12} more</div>` : '');
    return;
  }
  const s = D().summary(c.blocks), gb = s.frames * side * side * 2 / 1e9, ms = LP().msPerFrame();
  el.innerHTML = `<div class="ok"><b>${s.blocks} blocks · ${s.frames} frames of ${side} × ${side} px · ${gb < 0.1 ? gb.toFixed(3) : gb.toFixed(1)} GB` +
    (ms > 0 ? ` · about ${Math.max(1, Math.round(s.frames * ms / 1000))} s at the last ${ms.toFixed(0)} ms/frame` : '') + '</b></div>' +
    s.lines.map(l => `<div class="ok">${esc(l)}</div>`).join('') + (s.more ? `<div class="ok">… ${s.more} more</div>` : '');
}
function open() {
  if (!LP() || !D() || !self.iscSettings) return;
  plan = LP().get();
  cat = catalog();
  box = self.iscSettings.dialog('', () => { closePick(); if (box) box.classList.remove('mda'); });
  if (!pick.isConnected) {
    document.body.append(pick);
    addEventListener('scroll', ev => { if (pk && !pick.contains(ev.target)) closePick(); }, true);   // (the list is fixed: it would stay)
  }
  box.classList.add('mda');
  render();
}
self.iscMdaEditor = { open, settingsOf };
})();
