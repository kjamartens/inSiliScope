// SPDX-License-Identifier: BSD-3-Clause
// Every setting survives a page reload: the controls of the settings (#side: the live panel, its column and the
// settings below the image) and of the View box (#viewPanel) are written to localStorage when changed and set back,
// with their events, once the page has loaded. Per browser; "Reset all to defaults" (☰) forgets them.
(() => {
  const PREFIX = 'isc.set.';
  const roots = () => ['side', 'viewPanel'].map(id => document.getElementById(id)).filter(Boolean);
  const skip = el => !el.id || el.disabled || el.readOnly || el.type === 'file' || el.type === 'button' || el.type === 'submit'
    || el.type === 'radio' || el.type === 'search' || el.closest('#lv_recPop')
    || el.id === 'mv_pixel-nm';   // (follows the optics; the Magnification it sets is kept)
  const controls = () => roots().flatMap(r => [...r.querySelectorAll('input, select, textarea')]).filter(el => !skip(el));
  const read = el => el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value;
  const write = (el, v) => { if (el.type === 'checkbox') el.checked = v === '1'; else el.value = v; };
  const ls = {
    get(k) { try { return localStorage.getItem(PREFIX + k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(PREFIX + k, v); } catch (e) { /* private window */ } },
    clear() { try { for (const k of Object.keys(localStorage)) if (k.startsWith(PREFIX)) localStorage.removeItem(k); } catch (e) { /* */ } },
  };
  let restoring = false;
  const save = ev => {
    const el = ev.target;
    if (restoring || !(el instanceof Element) || !el.matches('input, select, textarea') || skip(el)) return;
    if (!roots().some(r => r.contains(el))) return;
    ls.set(el.id, read(el));
  };
  document.addEventListener('change', save, true);
  document.addEventListener('input', save, true);
  window.addEventListener('isc-reset', () => ls.clear());

  function restore() {
    const fire = el => { el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); };
    restoring = true;
    try {
      // twice: a preset restored first may set its rows; the second pass puts the remembered rows back over it
      for (let pass = 0; pass < 2; pass++) {
        for (const el of controls()) {
          const v = ls.get(el.id);
          if (v === null || read(el) === v) continue;
          if (el.tagName === 'SELECT' && ![...el.options].some(o => o.value === v)) continue;
          write(el, v);
          fire(el);
        }
      }
    } finally { restoring = false; }
  }
  if (document.readyState === 'complete') setTimeout(restore, 0);
  else window.addEventListener('load', () => setTimeout(restore, 0));
})();
