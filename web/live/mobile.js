// SPDX-License-Identifier: BSD-3-Clause
// The viewer on a phone (a window at most 700 px wide; landscape phones and tablets keep the side-by-side layout):
// body.mobile, and the live image and the world view moved into one square stage (#mStage) at the top of the page,
// one of them filling it, the other an inset in its top-right corner (a tap on the inset swaps them; the choice is
// kept), with the live bar under it; both stay on screen (sticky) while the controls and settings below scroll with
// the page. Leaving the phone width (rotation, resize) puts every node back where it was. The phone's CSS is the
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

const moved = ['canvasHost', 'liveView', 'liveBar'].map(id => ({ el: $(id), parent: null, next: null }));
let on = false;
function enter() {
  for (const m of moved) { m.parent = m.el.parentNode; m.next = m.el.nextSibling; }
  const [host, live, bar] = moved.map(m => m.el);
  stage.prepend(live, host);
  top.append(bar);
  $('side').prepend(top);
}
function leave() {
  for (const m of moved.slice().reverse()) m.parent.insertBefore(m.el, m.next);
  top.remove();
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
