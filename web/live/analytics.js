// SPDX-License-Identifier: BSD-3-Clause
// Anonymous usage counts (Umami Cloud, cookieless; docs/privacy.md says what is counted and why no consent is asked).
// Only on the published site (kjamartens.github.io): local copies, the lab and the tests load nothing and send nothing;
// none either with Do Not Track or Global Privacy Control on, or with localStorage umami.disabled set (to leave out
// your own visits). iscTrack(name, data) sends one event; data holds only fixed values (option values of the
// viewer's own selects, booleans, buckets), never typed text or file names: clean() drops anything else.
// A pick counts only when a person makes it (a trusted change event): restored settings, presets setting other controls
// and Reset fire synthetic events, which are not counted. One summary of what was in use goes when the page closes.
// The docs carry the same tag (overrides/partials/integrations/analytics/custom.html); keep the website id in step.
(() => {
  const SITE = 'kjamartens.github.io', WEBSITE_ID = 'e83bad18-7e9d-4c4c-9fae-90f55826533e';
  const optedOut = () => {
    try { if (localStorage.getItem('umami.disabled')) return true; } catch (e) { /* private window */ }
    const n = typeof navigator !== 'undefined' ? navigator : {};
    return n.doNotTrack === '1' || (typeof window !== 'undefined' && window.doNotTrack === '1') || n.globalPrivacyControl === true;
  };
  const on = typeof location !== 'undefined' && location.hostname === SITE && !optedOut();

  // A string from a fixed list is at most 50 characters (Umami's limit for event names; plenty for a preset name).
  const clean = data => {
    const out = {};
    for (const [k, v] of Object.entries(data || {})) {
      if (typeof v === 'boolean' || (typeof v === 'number' && Number.isFinite(v))) out[k] = v;
      else if (typeof v === 'string' && v.length <= 50) out[k] = v;
    }
    return out;
  };
  // Counts in buckets, so no event carries an exact number someone typed.
  const bucket = (n, edges) => { for (const e of edges) if (n <= e) return '<=' + e; return '>' + edges[edges.length - 1]; };
  const framesBucket = n => bucket(n, [1, 10, 100, 1000, 10000]);

  const queue = [];
  const send = (name, data) => {
    const d = clean(data);
    if (window.umami && window.umami.track) window.umami.track(name, d); else queue.push([name, d]);
  };
  const USED = { live: 'live', record: 'recorded', 'save-tiff': 'saved', 'make-movie': 'movie' };
  const iscTrack = (name, data) => {
    if (USED[name]) used[USED[name]] = true;
    if (on) send(name, data);
  };

  // The selects whose choices count: by id, plus every select with data-pick (the Configuration's preset groups).
  const PICK_IDS = { mv_specimen: 'specimen', mv_modality: 'modality', theme: 'look' };
  // A numeric option value (the specimen, the modality) goes as its option's label.
  const optionValue = sel => {
    const o = sel.options[sel.selectedIndex];
    if (!o) return '';
    return /^-?\d+$/.test(o.value) ? o.textContent.trim() : o.value;
  };
  const pickOf = el => (el && el.tagName === 'SELECT') ? (PICK_IDS[el.id] || el.dataset.pick || '') : '';
  const level = () => (typeof document !== 'undefined' && document.body && document.body.dataset.level) || 'basic';

  // What the session used, for the summary.
  const used = { live: false, recorded: false, saved: false, movie: false };
  const t0 = typeof performance !== 'undefined' ? performance.now() : 0;
  const current = () => {
    const sel = q => document.querySelector(q);
    const sp = sel('#mv_specimen'), mode = sel('select[data-pick="mode"]'), look = sel('#theme');
    return { specimen: sp ? optionValue(sp) : '', mode: mode ? (mode.value || 'custom') : '', look: look ? look.value : '',
      level: level() };
  };

  if (on) {
    const s = document.createElement('script');
    s.defer = true;
    s.src = 'https://cloud.umami.is/script.js';
    Object.assign(s.dataset, { websiteId: WEBSITE_ID, domains: SITE, doNotTrack: 'true', excludeSearch: 'true', excludeHash: 'true' });
    s.onload = () => { for (const [n, d] of queue.splice(0)) window.umami.track(n, d); };
    document.head.append(s);

    document.addEventListener('change', ev => {
      const what = ev.isTrusted && pickOf(ev.target);
      if (what) send('pick', { control: what, value: optionValue(ev.target) });
    }, true);
    document.addEventListener('click', ev => {
      const b = ev.isTrusted && ev.target.closest && ev.target.closest('#levelSeg button[data-level]');
      if (b) send('pick', { control: 'level', value: b.dataset.level });
    }, true);
    let summarized = false;
    window.addEventListener('pagehide', () => {
      if (summarized) return;
      summarized = true;
      const min = (performance.now() - t0) / 60000;
      send('summary', { ...current(), ...used, minutes: bucket(min, [1, 5, 15, 30, 60]) });
    });
  }

  self.iscTrack = iscTrack;
  self.iscTrack.framesBucket = framesBucket;
  // for tests/web/analytics_unit.mjs
  self.iscTrack._test = { clean, bucket, pickOf, optionValue, on };
})();
