// SPDX-License-Identifier: BSD-3-Clause
// A typed value takes effect once it has settled: the keystrokes in a number or text box reach the page 0.5 s after
// the last one, or at once on Enter or when the box loses focus -- so typing 200 never renders at 2 or 20 on the way.
// Two parts: the box's input events are held back, and while they are, its `value` (and `valueAsNumber`) read by the
// page's code is the last settled one -- the live loop and the scale bar read the boxes every frame, not on events.
// Sliders, selects, checkboxes, the mouse wheel and values set by the page itself act at once (only typed input
// events, `isTrusted`, wait). Listens at the window in the capture phase, before every other handler.
(() => {
  const SETTLE_MS = 500;
  const V = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
  const N = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'valueAsNumber');
  const pending = new Map();   // box -> timer
  const before = new WeakMap();   // box -> its value before the keystroke (the settled one)
  const typed = el => el instanceof HTMLInputElement && (el.type === 'number' || el.type === 'text');
  function hold(el, settled) {   // the page reads the settled value until commit; a value the page sets ends the hold
    Object.defineProperty(el, 'value', {
      configurable: true,
      get() { return settled; },
      set(v) { release(el); V.set.call(el, v); },
    });
    Object.defineProperty(el, 'valueAsNumber', {
      configurable: true,
      get() { const x = parseFloat(settled); return Number.isFinite(x) ? x : NaN; },
      set(v) { release(el); N.set.call(el, v); },
    });
  }
  function release(el) {
    const t = pending.get(el);
    if (t === undefined) return false;
    clearTimeout(t); pending.delete(el);
    delete el.value; delete el.valueAsNumber;
    return true;
  }
  function commit(el) {
    if (release(el)) el.dispatchEvent(new Event('input', { bubbles: true }));
  }
  window.addEventListener('keydown', ev => {
    const el = ev.target;
    if (!typed(el)) return;
    if (ev.key === 'Enter') { commit(el); return; }
    if (!pending.has(el)) before.set(el, V.get.call(el));
  }, true);
  window.addEventListener('input', ev => {
    const el = ev.target;
    if (!ev.isTrusted || !typed(el)) return;
    ev.stopImmediatePropagation();
    if (!pending.has(el)) hold(el, before.has(el) ? before.get(el) : V.get.call(el));
    clearTimeout(pending.get(el));
    pending.set(el, setTimeout(() => commit(el), SETTLE_MS));
  }, true);
  // leaving the box commits now; so does its own change event (fired on Enter / blur), before the change goes on
  window.addEventListener('change', ev => { if (typed(ev.target)) commit(ev.target); }, true);
  window.addEventListener('blur', ev => { if (typed(ev.target)) commit(ev.target); }, true);
})();
