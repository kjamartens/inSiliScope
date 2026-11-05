#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's design rules that a machine can check (docs/dev/DESIGN.md), in < 1 s:
//   1. web/design/tokens.css is what tools/gen_design_tokens.mjs makes of the token files;
//   2. no colour literal (#rgb, rgb(), hsl()) and no font-size in px outside the tokens: in web/index.html's <style> and
//      in the page's own scripts listed below (a line with `design:allow` and a reason is let through: physics and data
//      colours, a fallback);
//   3. WCAG 2.2 contrast of the semantic pairs, in the Dark and the Light theme (text 4.5:1, controls and focus 3:1).
// Run: node tests/web/design_lint.mjs  (CI job viewer-js)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { generate, resolve } from '../../tools/gen_design_tokens.mjs';

const t0 = performance.now();
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const rd = f => fs.readFileSync(path.join(ROOT, f), 'utf8').replace(/\r\n/g, '\n');
let bad = 0;
const fail = m => { bad++; console.log('FAIL  ' + m); };
const ok = m => console.log('ok    ' + m);

// 1. generated CSS up to date
if (rd('web/design/tokens.css') === generate()) ok('web/design/tokens.css is up to date');
else fail('web/design/tokens.css is out of date: node tools/gen_design_tokens.mjs');

// 2. literals
const COLOUR = /#[0-9a-fA-F]{3,8}\b(?![\w-])|\brgba?\(\s*\d|\bhsla?\(\s*\d/;
const FONT_PX = /font-size\s*:\s*\d/;
function scan(file, text, firstLine = 1) {
  const lines = text.split('\n');
  let n = 0;
  lines.forEach((l, i) => {
    if (/design:allow/.test(l) || (i > 0 && /design:allow/.test(lines[i - 1]) && /^\s*\/\//.test(lines[i - 1]))) return;
    const code = l.replace(/&#\d+;|&#x[0-9a-f]+;/gi, '');   // (HTML entities are not colours)
    if (COLOUR.test(code) || FONT_PX.test(code)) { n++; if (n <= 8) fail(`${file}:${firstLine + i}: a literal colour or font size: ${l.trim().slice(0, 120)}`); }
  });
  if (n > 8) fail(`${file}: ${n - 8} more`);
  if (!n) ok(`${file}: no colour or font-size literals`);
}
const html = rd('web/index.html');
const st = html.indexOf('<style>'), en = html.indexOf('</style>');
scan('web/index.html <style>', html.slice(st, en), html.slice(0, st).split('\n').length);
for (const f of ['web/design/theme.js', 'web/live/live.js', 'web/live/mobile.js', 'web/live/scope_panels.js',
  'web/live/settings_file.js', 'web/live/mm_config_ui.js']) scan(f, rd(f));

// 3. contrast
function rgb(c, bg) {
  c = c.trim();
  let m = /^#([0-9a-f]{3,8})$/i.exec(c);
  if (m) {
    let h = m[1];
    if (h.length <= 4) h = [...h].map(x => x + x).join('');
    const v = [0, 2, 4].map(i => parseInt(h.slice(i, i + 2), 16));
    const a = h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1;
    return a < 1 && bg ? v.map((x, i) => x * a + bg[i] * (1 - a)) : v;
  }
  m = /^rgba?\(([^)]+)\)$/.exec(c);
  if (m) {
    const p = m[1].split(',').map(Number), a = p.length > 3 ? p[3] : 1;
    return a < 1 && bg ? p.slice(0, 3).map((x, i) => x * a + bg[i] * (1 - a)) : p.slice(0, 3);
  }
  throw new Error('cannot read colour ' + c);
}
const lum = v => { const [r, g, b] = v.map(x => { x /= 255; return x <= 0.03928 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; }); return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
const ratio = (a, b) => { const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p); return (x + 0.05) / (y + 0.05); };
const PAIRS = [   // [foreground, background, minimum, what]
  ['--color-text', '--color-surface', 4.5, 'text'], ['--color-text-muted', '--color-surface', 4.5, 'muted text'],
  ['--color-text-muted', '--color-surface-raised', 4.5, 'muted text in a pop-over'], ['--color-text', '--color-input-bg', 4.5, 'an input\'s text'],
  ['--color-text-muted', '--color-input-bg', 4.5, 'a segmented control\'s text'], ['--color-link', '--color-surface', 4.5, 'a link'],
  ['--color-accent', '--color-surface', 3, 'the accent (marks, a hovered name)'], ['--color-focus-ring', '--color-surface', 3, 'the focus ring'],
  ['--color-input-border', '--color-input-bg', 3, 'an input\'s border'], ['--color-status-live', '--color-surface', 3, 'Live\'s outline'],
  ['--color-status-on-live', '--color-status-live-on', 4.5, 'Live\'s text while on'], ['--color-status-warn', '--color-surface', 3, 'a warning'],
  ['--color-overlay-text', '--color-bg-canvas', 4.5, 'text over an image'], ['--color-panel-text', '--color-surface', 4.5, 'a drawing\'s names'],
  ['--color-panel-muted', '--color-surface', 3, 'a drawing\'s values'],
];
const accents = [undefined, ...['teal', 'violet', 'amber', 'magenta']];
for (const theme of ['dark', 'light']) for (const acc of accents) {
  const T = resolve(theme, 'comfortable', acc && `{color.${acc}.${theme === 'dark' ? 400 : 600}}`);
  for (const [fg, bg, min, what] of PAIRS) {
    if (acc && !/accent|focus/.test(fg)) continue;   // the other accents: only the pairs the accent is in
    const b = rgb(T[bg]), r = ratio(rgb(T[fg], b), b);
    const name = `${theme}${acc ? ' + ' + acc : ''}: ${what} ${r.toFixed(2)}:1 (>= ${min})`;
    if (r + 1e-9 < min) fail(name); else if (!acc) ok(name);
  }
}

console.log(bad ? `design lint: ${bad} failure(s), ${((performance.now() - t0) / 1000).toFixed(2)} s`
  : `design lint: all ok, ${((performance.now() - t0) / 1000).toFixed(2)} s`);
process.exit(bad ? 1 : 0);
