// SPDX-License-Identifier: BSD-3-Clause
// The viewer on a phone (a window at most 700 px wide; landscape phones and tablets keep the side-by-side layout):
// body.mobile, and the live image and the world view moved into one square stage (#mStage) at the top of the page,
// one of them filling it, the other an inset in its top-right corner (a tap on the inset swaps them; the choice is
// kept), with the live bar under it; both stay on screen (sticky) while the controls and settings below scroll with
// the page. The Focus box has a knob instead of the focus field. Leaving the phone width (rotation, resize) puts every node back where it was. The phone's CSS is the
// body.mobile block in web/index.html.
//
// Loaded right after the viewer's DOM, before its scripts, so the canvases start in their phone places; later
// switches send a window resize, on which the world view (resizeCanvas) and the live view (zoom, scale bar) re-measure.
'use strict';
(function () {
const $ = id => document.getElementById(id);
const NARROW = matchMedia('(max-width:700px)');
const KEY = 'isc.ui.mobileBig';
const get = () => { try { return localStorage.getItem(KEY); } catch (e) { return null; } };
const set = v => { try { localStorage.setItem(KEY, v); } catch (e) { /* private window */ } };

const top = document.createElement('div'), stage = document.createElement('div'), swap = document.createElement('button');
top.id = 'mTop'; stage.id = 'mStage'; swap.id = 'mSwap';
swap.type = 'button';
swap.setAttribute('aria-label', 'Swap the live image and the world view');
swap.title = 'Swap the live image and the world view';
stage.append(swap);
top.append(stage);

// ---- the focus knob (the Focus box, in place of the focus field): a ruler that scrolls like a fine-focus knob (values grow to the right: swipe
// left to focus up), one step of the step size per KNOB_PX, a fast swipe several; − / + at its ends step once and
// repeat while held. Two fingers up or down on the live image move the focus too (live.js).
const KNOB_PX = 12;
const knob = document.createElement('div');
knob.id = 'mFocus';
knob.innerHTML = '<button type="button" data-dir="-1" aria-label="Focus down" title="Focus down one step (hold to repeat)">−</button>'
  + '<canvas id="mKnob" aria-label="Focus: swipe left to focus up, right to focus down"></canvas>'
  + '<span id="mZ"></span><button type="button" data-dir="1" aria-label="Focus up" title="Focus up one step (hold to repeat)">+</button>';
const kcv = knob.querySelector('canvas');
const zStep = () => Math.max(0.001, +$('lv_zStep').value || 0.1);
const step = dir => { if (self.liveState && liveState.stepZ) liveState.stepZ(dir); };
function drawKnob() {
  if (!on || !$('mv_z')) return;
  const dpr = devicePixelRatio, W = Math.round(kcv.clientWidth * dpr), H = Math.round(kcv.clientHeight * dpr);
  if (!W || !H) return;
  if (kcv.width !== W || kcv.height !== H) { kcv.width = W; kcv.height = H; }
  const g = kcv.getContext('2d'), cs = getComputedStyle(document.body);
  const z = +$('mv_z').value || 0, st = zStep(), at = z / st, cx = W / 2, sp = KNOB_PX * dpr;
  g.clearRect(0, 0, W, H);
  g.fillStyle = cs.getPropertyValue('--muted') || '#adb8c8';
  g.font = `${10 * dpr}px system-ui,sans-serif`; g.textAlign = 'center'; g.textBaseline = 'top';
  for (let k = Math.floor(at - cx / sp) - 1; k <= Math.ceil(at + cx / sp) + 1; k++) {
    const x = Math.round(cx + (k - at) * sp) + 0.5, major = k % 10 === 0, mid = k % 5 === 0;
    g.fillRect(x - 0.5 * dpr, H - (major ? 16 : mid ? 11 : 7) * dpr, dpr, (major ? 16 : mid ? 11 : 7) * dpr);
    if (major) g.fillText(+(k * st).toFixed(3) + '', x, 2 * dpr);
  }
  g.fillStyle = cs.getPropertyValue('--accent') || '#559ded';
  g.fillRect(cx - dpr, 0, 2 * dpr, H);
  $('mZ').textContent = z.toFixed(st < 0.01 ? 3 : 2) + ' µm';
}
{
  let drag = null;
  kcv.addEventListener('pointerdown', ev => { drag = { x: ev.clientX, t: ev.timeStamp, acc: 0 }; kcv.setPointerCapture(ev.pointerId); });
  kcv.addEventListener('pointermove', ev => {
    if (!drag) return;
    const dx = ev.clientX - drag.x, dt = Math.max(1, ev.timeStamp - drag.t), v = Math.abs(dx) / dt;   // px per ms
    const gain = v > 0.6 ? Math.min(8, 1 + (v - 0.6) * 6) : 1;   // a fast swipe moves further
    drag.x = ev.clientX; drag.t = ev.timeStamp;
    drag.acc -= dx * gain;
    while (Math.abs(drag.acc) >= KNOB_PX) { const s = Math.sign(drag.acc); step(s); drag.acc -= s * KNOB_PX; }
  });
  const end = () => { drag = null; };
  kcv.addEventListener('pointerup', end); kcv.addEventListener('pointercancel', end);
}
for (const b of knob.querySelectorAll('button')) {
  let timer = 0;
  const stop = () => { clearTimeout(timer); clearInterval(timer); timer = 0; };
  b.addEventListener('pointerdown', ev => {
    ev.preventDefault(); stop();
    const dir = +b.dataset.dir;
    step(dir);
    timer = setTimeout(() => { timer = setInterval(() => step(dir), 90); }, 420);
  });
  for (const e of ['pointerup', 'pointerleave', 'pointercancel']) b.addEventListener(e, stop);
  b.addEventListener('contextmenu', ev => ev.preventDefault());   // (a long press would open it)
}
// (the focus setting mv_z is made by the viewer's scripts, after this one)
for (const e of ['input', 'change']) document.addEventListener(e, ev => { if (/^(mv_z|lv_z|lv_zStep)$/.test(ev.target.id || '')) drawKnob(); }, true);
window.addEventListener('resize', drawKnob);
window.addEventListener('load', drawKnob);

const moved = ['canvasHost', 'liveView', 'liveBar'].map(id => ({ el: $(id), parent: null, next: null }));
let on = false;
function enter() {
  for (const m of moved) { m.parent = m.el.parentNode; m.next = m.el.nextSibling; }
  const [host, live, bar] = moved.map(m => m.el);
  stage.prepend(live, host);
  top.append(bar);
  $('side').prepend(top);
  $('lv_z').closest('.zrow').before(knob);   // in the Focus box, in place of the focus field
}
function leave() {
  for (const m of moved.slice().reverse()) m.parent.insertBefore(m.el, m.next);
  top.remove();
  knob.remove();
}
function showBig(which) {
  stage.classList.toggle('worldBig', which === 'world');
  set(which);
}
function apply(first) {
  const want = NARROW.matches;
  if (want === on) return;
  on = want;
  document.body.classList.toggle('mobile', on);
  if (on) enter(); else leave();
  if (!first) window.dispatchEvent(new Event('resize'));
  else if (on) requestAnimationFrame(drawKnob);
}
swap.addEventListener('click', () => {
  showBig(stage.classList.contains('worldBig') ? 'live' : 'world');
  window.dispatchEvent(new Event('resize'));
});
stage.classList.toggle('worldBig', get() === 'world');
apply(true);
NARROW.addEventListener('change', () => apply(false));
self.iscMobile = { get on() { return on; }, get worldInset() { return on && !stage.classList.contains('worldBig'); }, showBig: w => { showBig(w); window.dispatchEvent(new Event('resize')); } };
})();
