// SPDX-License-Identifier: BSD-3-Clause
// The viewer's theme at run time (docs/dev/DESIGN.md): the colour scheme (dark | light | system) and the density
// (comfortable | compact) as attributes on <html> that web/design/tokens.css keys on, kept in localStorage
// (isc.ui.theme, isc.ui.density); the hidden accent override; tok(), a token's value for canvas code. Loaded in <head>
// before the page draws (no flash of the wrong theme). Fires 'isc-theme' on window whenever what a token resolves to
// may have changed (a theme, density or accent change, or the system's scheme under "system"): canvases redraw on it.
(function () {
  'use strict';
  const root = document.documentElement;
  const THEMES = ['dark', 'light', 'system'], DENSITIES = ['comfortable', 'compact'];
  const PALETTES = ['blue', 'teal', 'violet', 'amber', 'magenta'];
  const get = (k, d) => { try { const v = localStorage.getItem('isc.ui.' + k); return v === null ? d : v; } catch (e) { return d; } };
  const set = (k, v) => { try { localStorage.setItem('isc.ui.' + k, v); } catch (e) { /* private window */ } };

  // (before 2026-11-05 one "style" held both: focus | compact | light)
  const old = get('style', null);
  if (old !== null && get('theme', null) === null) {
    set('theme', old === 'light' ? 'light' : 'dark');
    set('density', old === 'compact' ? 'compact' : 'comfortable');
  }

  let cache = null;
  const changed = () => { cache = null; window.dispatchEvent(new Event('isc-theme')); };

  function setTheme(v, quiet) {
    if (!THEMES.includes(v)) v = 'dark';
    root.dataset.theme = v;
    if (!quiet) set('theme', v);
    changed();
  }
  function setDensity(v, quiet) {
    if (!DENSITIES.includes(v)) v = 'comfortable';
    root.dataset.density = v;
    if (!quiet) set('density', v);
    changed();
  }
  // The accent override, for trying a colour without editing web/design/tokens/accent.tokens.json: a palette name
  // (?accent=teal) or a colour (?accent=%23e66a00), from the URL or localStorage isc.ui.accent. Not offered in the UI.
  function setAccent(v) {
    const s = root.style;
    if (!v) { s.removeProperty('--accent-dark'); s.removeProperty('--accent-light'); }
    else if (PALETTES.includes(v)) { s.setProperty('--accent-dark', `var(--color-${v}-400)`); s.setProperty('--accent-light', `var(--color-${v}-600)`); }
    else if (CSS.supports('color', v)) { s.setProperty('--accent-dark', v); s.setProperty('--accent-light', v); }
    changed();
  }

  // the value of a token (a CSS custom property, e.g. tok('--color-text')), as resolved now
  function tok(name) {
    if (!cache) cache = new Map();
    if (!cache.has(name)) cache.set(name, getComputedStyle(root).getPropertyValue(name).trim());
    return cache.get(name);
  }
  // the theme in effect: light or dark ("system" resolved)
  const isLight = () => root.dataset.theme === 'light' ||
    (root.dataset.theme === 'system' && matchMedia('(prefers-color-scheme: light)').matches);

  root.dataset.theme = THEMES.includes(get('theme', 'dark')) ? get('theme', 'dark') : 'dark';
  root.dataset.density = DENSITIES.includes(get('density', 'comfortable')) ? get('density', 'comfortable') : 'comfortable';
  let acc = null;
  try { acc = new URLSearchParams(location.search).get('accent'); } catch (e) { /* no URL */ }
  if (acc) setAccent(acc); else if (get('accent', '')) setAccent(get('accent', ''));
  matchMedia('(prefers-color-scheme: light)').addEventListener('change', () => { if (root.dataset.theme === 'system') changed(); });

  self.iscTheme = {
    THEMES, DENSITIES, tok, isLight, setTheme, setDensity, setAccent,
    theme: () => root.dataset.theme, density: () => root.dataset.density,
  };
})();
