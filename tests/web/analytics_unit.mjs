#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// web/live/analytics.js in Node on a stub page: off the published site, with Do Not Track, Global Privacy Control or
// umami.disabled it loads and sends nothing; on it, the Umami tag carries the site's attributes, events wait for the
// tag, a pick counts only from a person (a trusted change of a counted select; never a restored or preset change, a
// text field or a select that is not counted), the level buttons count, the summary goes once, and event data keeps
// only short strings, numbers and booleans.
//   node tests/web/analytics_unit.mjs
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SRC = fs.readFileSync(path.join(ROOT, 'web/live/analytics.js'), 'utf8');
const t0 = Date.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };

// A page: its listeners, the scripts appended to <head>, the events Umami got.
function page({ host = 'kjamartens.github.io', dnt = null, gpc = false, disabled = false } = {}) {
  const listeners = {}, scripts = [], sent = [];
  const on = (t, f) => { (listeners[t] = listeners[t] || []).push(f); };
  const els = {};
  const document = {
    head: { append: s => scripts.push(s) },
    body: { dataset: { level: 'advanced' } },
    createElement: () => ({ dataset: {} }),
    addEventListener: on,
    querySelector: q => els[q] || null,
  };
  const window = { addEventListener: on, doNotTrack: null };
  const g = { location: { hostname: host }, navigator: { doNotTrack: dnt, globalPrivacyControl: gpc }, document, window,
    localStorage: { getItem: k => (disabled && k === 'umami.disabled' ? '1' : null) }, performance: { now: () => 0 } };
  g.self = g;
  vm.createContext(g);
  vm.runInContext(SRC, g);
  const fire = (t, ev) => (listeners[t] || []).forEach(f => f(ev));
  const umamiLoads = () => { window.umami = { track: (n, d) => sent.push([n, d]) }; scripts[0].onload(); };
  return { g, scripts, sent, fire, els, umamiLoads };
}
const select = (id, options, value, extra = {}) => {
  const opts = options.map(([v, t]) => ({ value: v, textContent: t }));
  return { tagName: 'SELECT', id, dataset: {}, options: opts, selectedIndex: opts.findIndex(o => o.value === value), value, ...extra };
};

// off the site, or opted out: no tag, no listener, nothing sent
for (const [what, o] of [['localhost', { host: 'localhost' }], ['a fork', { host: 'someone.github.io' }],
  ['Do Not Track', { dnt: '1' }], ['Global Privacy Control', { gpc: true }], ['umami.disabled', { disabled: true }]]) {
  const p = page(o);
  p.g.iscTrack('live');
  check(p.scripts.length === 0 && !p.g.iscTrack._test.on, `${what}: no Umami tag, iscTrack does nothing`);
}

const p = page();
const s = p.scripts[0];
check(p.scripts.length === 1 && s.src === 'https://cloud.umami.is/script.js' && s.defer, 'on the site: one deferred Umami tag');
check(s.dataset.websiteId === 'e83bad18-7e9d-4c4c-9fae-90f55826533e' && s.dataset.domains === 'kjamartens.github.io'
  && s.dataset.doNotTrack === 'true' && s.dataset.excludeSearch === 'true' && s.dataset.excludeHash === 'true',
  'the tag: website id, domains, do-not-track, no search or hash');
const overrides = fs.readFileSync(path.join(ROOT, 'overrides/partials/integrations/analytics/custom.html'), 'utf8');
check(overrides.includes(s.dataset.websiteId) && overrides.includes("'kjamartens.github.io'"), 'the docs tag has the same website id and site');

p.g.iscTrack('live');
check(p.sent.length === 0, 'an event before the tag has loaded waits');
p.umamiLoads();
check(p.sent.length === 1 && p.sent[0][0] === 'live', 'and goes when it has');

const spec = select('mv_specimen', [['0', 'Cell field'], ['1', 'Nanorulers']], '1');
const mode = select('', [['', '—'], ['ATTO655 DNA-PAINT', 'DNA-PAINT']], 'ATTO655 DNA-PAINT', { dataset: { pick: 'mode' } });
const other = select('mtMap', [['rainbow', 'Rainbow']], 'rainbow');
const text = { tagName: 'INPUT', id: 'mv_specimen', dataset: { pick: 'x' }, value: 'typed text' };
p.sent.length = 0;
p.fire('change', { isTrusted: true, target: spec });
p.fire('change', { isTrusted: true, target: mode });
check(JSON.stringify(p.sent) === JSON.stringify([['pick', { control: 'specimen', value: 'Nanorulers' }], ['pick', { control: 'mode', value: 'ATTO655 DNA-PAINT' }]]),
  'a person\'s pick: the control and its option (a numeric value as its label)');
p.sent.length = 0;
p.fire('change', { isTrusted: false, target: spec });
p.fire('change', { isTrusted: true, target: other });
p.fire('change', { isTrusted: true, target: text });
check(p.sent.length === 0, 'not counted: a synthetic change (restore, preset, reset), a select not counted, a text field');

const levelBtn = { dataset: { level: 'expert' } };
p.fire('click', { isTrusted: true, target: { closest: q => (q.includes('levelSeg') ? levelBtn : null) } });
p.fire('click', { isTrusted: false, target: { closest: q => (q.includes('levelSeg') ? levelBtn : null) } });
check(JSON.stringify(p.sent) === JSON.stringify([['pick', { control: 'level', value: 'expert' }]]), 'the level buttons: a person\'s click counts');

p.sent.length = 0;
Object.assign(p.els, { '#mv_specimen': spec, 'select[data-pick="mode"]': mode, '#theme': { value: 'flat' } });
p.g.iscTrack('record', { frames: '<=100', continue: false });
p.sent.length = 0;
p.fire('pagehide', {});
p.fire('pagehide', {});
const sum = p.sent.length === 1 && p.sent[0][0] === 'summary' && p.sent[0][1];
check(sum && sum.specimen === 'Nanorulers' && sum.mode === 'ATTO655 DNA-PAINT' && sum.look === 'flat' && sum.level === 'advanced'
  && sum.recorded === true && sum.live === true && sum.saved === false && sum.minutes === '<=1', 'one summary when the page closes: what was in use');

const { clean, bucket } = p.g.iscTrack._test;
check(JSON.stringify(clean({ a: 'x', b: 'y'.repeat(51), c: { d: 1 }, e: 2, f: true, g: NaN, h: null })) === '{"a":"x","e":2,"f":true}',
  'event data: long strings, objects, NaN and null dropped');
check(bucket(0, [1, 10]) === '<=1' && bucket(10, [1, 10]) === '<=10' && bucket(11, [1, 10]) === '>10'
  && p.g.iscTrack.framesBucket(300) === '<=1000', 'counts go in buckets');

console.log(`${fail ? 'FAIL' : 'ok'}  analytics_unit  ${Date.now() - t0} ms`);
process.exit(fail ? 1 : 0);
