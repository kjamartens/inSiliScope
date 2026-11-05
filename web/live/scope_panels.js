// The Microscope view's panels (2026-11-03): draws a panel the module describes (cli/scope_panels.h: a JSON header of
// typed views and the floats they point into) without knowing which panel it is -- images with overlays, curves,
// rows of text and the drawn light path. Page-free: the viewer's Microscope view (web/index.html) and the docs'
// interactive figure (web/illumination.html) both draw with it; a new panel needs only the C++.
(function (root) {
  'use strict';

  // Calls the module's isc_scope_panel (the sizing call, then the filling call) and unpacks it: {header, data} or
  // {error}. params: "name=value ..."; scale 1 (a tile) or 2 (filling the view).
  function readPanel(M, spec, id, params, scale) {
    if (!M._isc_scope_panel) return { error: 'this module has no panels (regenerate web/insiliscope_module.js)' };
    const str = t => { const n = M.lengthBytesUTF8(t) + 1, p = M._malloc(n); M.stringToUTF8(t, p, n); return p; };
    const s = str(spec), i = str(id), pr = str(params || ''), sizes = M._malloc(8), err = M._malloc(512);
    let hb = 0, fb = 0, res;
    try {
      if (M._isc_scope_panel(s, i, pr, scale || 1, 0, 0, 0, 0, sizes, err, 512) < 0) return { error: M.UTF8ToString(err) };
      const hn = M.HEAP32[sizes / 4], fn = M.HEAP32[sizes / 4 + 1];
      hb = M._malloc(hn + 1); fb = M._malloc(Math.max(4, fn * 4));
      if (M._isc_scope_panel(s, i, pr, scale || 1, hb, hn + 1, fb, fn, sizes, err, 512) < 0) return { error: M.UTF8ToString(err) };
      res = { header: JSON.parse(M.UTF8ToString(hb)), data: M.HEAPF32.slice(fb / 4, fb / 4 + fn) };
    } finally {
      for (const p of [s, i, pr, sizes, err, hb, fb]) if (p) M._free(p);
    }
    return res;
  }
  // The panel list: [{id, title, slot}].
  function readPanelList(M) {
    if (!M._isc_scope_panels) return [];
    const n = M._isc_scope_panels(0, 0), b = M._malloc(n + 1);
    M._isc_scope_panels(b, n + 1);
    const list = JSON.parse(M.UTF8ToString(b));
    M._free(b);
    return list;
  }

  // A wavelength's colour (violet 400 nm .. red 700 nm, beyond it red): bright on the dark looks, darker on Light;
  // the hue follows the spectrum piecewise (400 violet, 450 blue, 490 cyan, 520 green, 570 yellow, 590 orange,
  // >= 625 red).
  const WAVE_HUE = [[400, 275], [450, 235], [490, 185], [520, 125], [570, 58], [590, 32], [625, 2]];
  // design:allow -- a wavelength's own colour (physics, not the theme): a hue per nm
  function waveColour(nm, light) {
    let hue = WAVE_HUE[WAVE_HUE.length - 1][1];
    if (nm <= WAVE_HUE[0][0]) hue = WAVE_HUE[0][1];
    else for (let i = 1; i < WAVE_HUE.length; i++) if (nm <= WAVE_HUE[i][0]) {
      const [a, ha] = WAVE_HUE[i - 1], [b, hb] = WAVE_HUE[i];
      hue = ha + (hb - ha) * (nm - a) / (b - a); break;
    }
    return light ? `hsl(${hue.toFixed(0)},85%,42%)` : `hsl(${hue.toFixed(0)},100%,${nm >= 625 ? 66 : 62}%)`;   // design:allow
  }

  // ---- images ----
  // The theme's colours (web/design/tokens.css, docs/dev/DESIGN.md) as they resolve now: tok('panel-ink') = --color-panel-ink.
  // (Not a theme of their own: they read the page's; the panels redraw on 'isc-theme'.)
  const tok = n => (self.iscTheme ? self.iscTheme.tok('--color-' + n) : '') || '#888';   // design:allow -- the fallback before theme.js
  // The heat map: dark blue-black -> orange -> white (design:allow: a data colour map).
  const RAMP = (() => {
    const r = new Uint8Array(256 * 3);
    for (let i = 0; i < 256; i++) {
      const t = i / 255;
      r[3 * i] = Math.round(255 * Math.min(1, Math.max(0, 1.6 * t)));
      r[3 * i + 1] = Math.round(255 * Math.min(1, Math.max(0, 1.6 * t - 0.45)));
      r[3 * i + 2] = Math.round(255 * Math.min(1, Math.max(0, t < 0.25 ? 1.2 * t + 0.1 : 2.2 * t - 1.2)));
    }
    return r;
  })();
  const peakOf = (data, v) => { let m = 0; for (let i = v.offset, e = v.offset + v.len; i < e; i++) if (data[i] > m) m = data[i]; return m; };
  const roleColour = r => tok(r === 'evanescent' ? 'panel-evanescent' : r === 'focus' ? 'accent' : 'panel-line');
  // An image at the canvas's size on the page (its pixels scaled up or down, pixelated ones without smoothing), the
  // overlays over it in screen pixels: an image stretched one way (an x-z section of 71 planes) keeps even lines and
  // dashes. Before the canvas is laid out, at the image's own size.
  function drawImage(canvas, v, data, peak) {
    const src = document.createElement('canvas');
    src.width = v.w; src.height = v.h;
    const gs = src.getContext('2d'), img = gs.createImageData(v.w, v.h), px = img.data;
    const k = peak > 0 ? 1 / peak : 0, grey = v.map === 'grey', gam = v.gamma || 1;
    for (let i = 0, n = v.w * v.h; i < n; i++) {
      let t = Math.max(0, Math.min(1, data[v.offset + i] * k));
      if (gam !== 1) t = Math.pow(t, gam);
      const c = Math.round(255 * t), o = 4 * i;
      if (grey) { px[o] = px[o + 1] = px[o + 2] = c; }
      else { px[o] = RAMP[3 * c]; px[o + 1] = RAMP[3 * c + 1]; px[o + 2] = RAMP[3 * c + 2]; }
      px[o + 3] = 255;
    }
    gs.putImageData(img, 0, 0);
    canvas._img = { v, src };
    renderImage(canvas);
  }

  // The image kept on a canvas (drawImage) at the canvas's size on the page and its zoom: the part shown scaled up to
  // the whole canvas (pixelated ones without smoothing), the overlays over it in screen pixels. An image made for a
  // part of the whole (v.window: x0, y0, span as fractions of the whole as shown; a resampled zoom) is placed there;
  // until the part now shown arrives, what it covers of it is drawn, the rest stays black.
  function renderImage(canvas) {
    const st = canvas._img;
    if (!st) return;
    const { v, src } = st, Z = canvas._zoom || { z: 1, x: 0, y: 0 };
    canvas._zoomView = v;   // the view drawn now (attachZoom's handlers read it)
    const dpr = (typeof devicePixelRatio === 'number' && devicePixelRatio) || 1, cw = canvas.clientWidth;
    const aspect = v.extent ? v.extent[0] / v.extent[1] : v.w / v.h;
    const W = cw > 0 ? Math.max(1, Math.min(2048, Math.round(cw * dpr))) : v.w;
    const H = cw > 0 ? Math.max(1, Math.min(2048, Math.round(cw * dpr / aspect))) : v.h;
    canvas.width = W; canvas.height = H;
    // a resampled image is made at the canvas's pixels, so it is drawn 1:1 (scaled, fringes a few pixels apart beat into
    // moire): a canvas of another size asks for its own once (viewChanged sends <name>.px)
    if (v.resample && cw > 0 && Math.abs(W - v.w) > 1 && canvas._askedPx !== W) {
      canvas._askedPx = W;
      setTimeout(() => viewChanged(canvas), 0);
    }
    const g = canvas.getContext('2d');
    g.imageSmoothingEnabled = !v.pixelated;
    if (!v.pixelated) g.imageSmoothingQuality = 'high';
    const [w0, h0, ws] = v.window || [0, 0, 1];
    // image units -> canvas pixels (through the fraction of the whole image)
    const sx = ws / v.w * Z.z * W, sy = ws / v.h * Z.z * H;
    const X = x => (w0 + x / v.w * ws - Z.x) * Z.z * W, Y = y => (h0 + y / v.h * ws - Z.y) * Z.z * H;
    g.fillStyle = tok('bg-canvas');
    g.fillRect(0, 0, W, H);
    // the part of the image on the canvas, clipped to the image (a source rect beyond it is not drawn by every browser)
    const ix0 = Math.max(0, (Z.x - w0) / ws * v.w), iy0 = Math.max(0, (Z.y - h0) / ws * v.h);
    const ix1 = Math.min(v.w, (Z.x + 1 / Z.z - w0) / ws * v.w), iy1 = Math.min(v.h, (Z.y + 1 / Z.z - h0) / ws * v.h);
    if (ix1 > ix0 && iy1 > iy0) g.drawImage(src, ix0, iy0, ix1 - ix0, iy1 - iy0, X(ix0), Y(iy0), X(ix1) - X(ix0), Y(iy1) - Y(iy0));
    const lw = cw > 0 ? Math.max(1.25, cw / 240) * dpr : Math.max(1, v.w / 200);
    if (v.scalebarUm > 0) scaleBar(g, W, H, v.scalebarUm / Z.z, dpr);
    for (const o of v.overlays || []) {
      g.strokeStyle = roleColour(o.role);
      // a dashed line marks a place (the cut, the focus): thin and half see-through, so the image shows through
      g.lineWidth = o.dash ? Math.max(dpr, 0.7 * lw) : lw;
      g.globalAlpha = o.dash ? 0.5 : 1;
      g.setLineDash(o.dash ? [3 * lw, 4 * lw] : []);
      g.beginPath();
      if (o.k === 'circle') g.ellipse(X(o.x), Y(o.y), o.r * sx, o.r * sy, 0, 0, 2 * Math.PI);
      else if (o.k === 'line') { g.moveTo(X(o.x1), Y(o.y1)); g.lineTo(X(o.x2), Y(o.y2)); }
      else if (o.k === 'rect') g.rect(X(o.x), Y(o.y), o.w * sx, o.h * sy);
      else if (o.k === 'arc') g.ellipse(X(o.x), Y(o.y), o.r * sx, o.r * sy, 0, o.a0, o.a1);
      g.stroke();
      if (o.k === 'arc' && o.arrow) {   // an arrowhead along the arc's direction at its end
        g.setLineDash([]);
        const hx = X(o.x + o.r * Math.cos(o.a1)), hy = Y(o.y + o.r * Math.sin(o.a1)), t = o.a1 + Math.PI / 2, h = 4 * lw;
        g.beginPath();
        g.moveTo(hx, hy); g.lineTo(hx - h * Math.cos(t) + h * 0.6 * Math.sin(t), hy - h * Math.sin(t) - h * 0.6 * Math.cos(t));
        g.moveTo(hx, hy); g.lineTo(hx - h * Math.cos(t) - h * 0.6 * Math.sin(t), hy - h * Math.sin(t) + h * 0.6 * Math.cos(t));
        g.stroke();
      }
    }
    g.setLineDash([]);
    g.globalAlpha = 1;
    // the inset: where the part shown sits in the whole image (hidden at 1x)
    const nav = canvas.parentElement && canvas.parentElement.querySelector('.pvNav');
    if (nav) {
      nav.hidden = Z.z === 1;
      nav.style.aspectRatio = aspect.toFixed(4);
      const s1 = 100 / Z.z;
      nav.firstChild.style.cssText = `left:${(100 * Z.x).toFixed(2)}%;top:${(100 * Z.y).toFixed(2)}%;width:${s1.toFixed(2)}%;height:${s1.toFixed(2)}%`;
    }
    canvas.style.cursor = Z.z === 1 ? '' : 'grab';
  }

  // A scale bar at the bottom right of a canvas W x H px showing umWide um across: a 1-2-5 length near a fifth of it,
  // white with a dark edge, its length above it.
  function scaleBar(g, W, H, umWide, dpr) {
    const want = umWide / 5, p = Math.pow(10, Math.floor(Math.log10(want))), m = want / p;
    const um = (m >= 5 ? 5 : m >= 2 ? 2 : 1) * p, len = um / umWide * W;
    const pad = 8 * dpr, th = Math.max(2, 3 * dpr), x1 = W - pad, x0 = x1 - len, y = H - pad;
    const label = um >= 1 ? +um.toPrecision(3) + ' \u00B5m' : Math.round(um * 1000) + ' nm';
    g.save();
    g.globalAlpha = 1;
    g.setLineDash([]);
    g.fillStyle = tok('overlay-bg');
    g.fillRect(x0 - dpr, y - th - dpr, len + 2 * dpr, th + 2 * dpr);
    g.fillStyle = tok('overlay-mark');
    g.fillRect(x0, y - th, len, th);
    g.font = `600 ${Math.round(11 * dpr)}px system-ui, sans-serif`;
    g.textAlign = 'center';
    g.textBaseline = 'bottom';
    g.lineWidth = 3 * dpr;
    g.strokeStyle = tok('overlay-shadow');
    g.strokeText(label, x0 + len / 2, y - th - 3 * dpr);
    g.fillText(label, x0 + len / 2, y - th - 3 * dpr);
    g.restore();
  }

  // A resampled image's zoom or size changed: the panel's host asks for the parts now shown at the canvases' pixels
  // (opts.onView, the page's request with host._viewParams: <name>.px per such image, and <name>.x0, .y0, .span per
  // such image zoomed in).
  function viewChanged(canvas) {
    const host = canvas.closest('.pnl');
    if (!host || !host._pLast) return;
    const parts = [];
    for (const c of host.querySelectorAll('.pv-image canvas')) {
      const v = c._zoomView, Z = c._zoom;
      if (!v || !v.resample) continue;
      if (c.width > 0) parts.push(`${v.name}.px=${c.width}`);
      if (!Z || Z.z === 1) continue;
      parts.push(`${v.name}.x0=${Z.x.toFixed(6)}`, `${v.name}.y0=${Z.y.toFixed(6)}`, `${v.name}.span=${(1 / Z.z).toFixed(6)}`);
    }
    host._viewParams = parts.join(' ');
    const o = host._pLast[1];
    if (o && o.onView) o.onView(host);
  }

  // Zoom for an image's canvas (v its view): the wheel zooms about the pointer (1..ZOOM_MAX; at 1x a wheel down scrolls
  // the page as usual), a drag pans the zoomed image (and then is no click: a tile opens on a click), a click on the
  // inset shows the whole image again. The zoom stays with the canvas across redraws (a slider moved keeps it). Images
  // with the same link (group) zoom together: the factor and the centre of the part shown, in the views' units (um).
  const ZOOM_MAX = 32;
  function clampZoom(Z) {
    Z.z = Math.min(ZOOM_MAX, Math.max(1, Z.z));
    if (Z.z < 1.01) Object.assign(Z, { z: 1, x: 0, y: 0 });
    const span = 1 / Z.z;
    Z.x = Math.min(1 - span, Math.max(0, Z.x)); Z.y = Math.min(1 - span, Math.max(0, Z.y));
  }
  function attachZoom(canvas, v, group) {
    const Z = canvas._zoom = { z: 1, x: 0, y: 0 };
    const extOf = v => v.extent || [v.w, v.h];
    canvas._zoomView = v;
    if (group) group.push(canvas);
    const nav = canvas.parentElement.querySelector('.pvNav');
    const apply = () => {
      clampZoom(Z);
      renderImage(canvas);
      const cur = canvas._zoomView;
      if (cur.resample) viewChanged(canvas);
      if (!group) return;
      // the others in the group: the same factor about the same point
      const [ew, eh] = extOf(cur), cu = (Z.x + 0.5 / Z.z - 0.5) * ew, cv = (Z.y + 0.5 / Z.z - 0.5) * eh;
      for (const m of group) if (m !== canvas) {
        const [mw, mh] = extOf(m._zoomView), M = m._zoom;
        M.z = Z.z; M.x = cu / mw + 0.5 - 0.5 / Z.z; M.y = cv / mh + 0.5 - 0.5 / Z.z;
        clampZoom(M);
        renderImage(m);
      }
    };
    canvas.addEventListener('wheel', ev => {
      if (Z.z === 1 && ev.deltaY > 0) return;
      ev.preventDefault();
      const r = canvas.getBoundingClientRect(), px = (ev.clientX - r.left) / r.width, py = (ev.clientY - r.top) / r.height;
      const u = Z.x + px / Z.z, w = Z.y + py / Z.z;   // the point under the pointer, as a fraction of the image
      Z.z *= Math.exp(-ev.deltaY * (ev.deltaMode === 1 ? 16 : 1) * 0.0025);
      const z = Math.min(ZOOM_MAX, Math.max(1, Z.z));
      Z.x = u - px / z; Z.y = w - py / z;
      apply();
    }, { passive: false });
    let drag = null, moved = false;
    canvas.addEventListener('pointerdown', ev => {
      moved = false;
      if (Z.z === 1 || ev.button !== 0) return;
      drag = { x: ev.clientX, y: ev.clientY, zx: Z.x, zy: Z.y };
      try { canvas.setPointerCapture(ev.pointerId); } catch (e) { /* a synthetic pointer */ }
      canvas.style.cursor = 'grabbing';
    });
    canvas.addEventListener('pointermove', ev => {
      if (!drag) return;
      const r = canvas.getBoundingClientRect(), dx = ev.clientX - drag.x, dy = ev.clientY - drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 3) moved = true;
      Z.x = drag.zx - dx / r.width / Z.z; Z.y = drag.zy - dy / r.height / Z.z;
      apply();
    });
    const end = () => { if (drag) { drag = null; canvas.style.cursor = Z.z === 1 ? '' : 'grab'; } };
    canvas.addEventListener('pointerup', end);
    canvas.addEventListener('pointercancel', end);
    canvas.addEventListener('click', ev => { if (moved) { ev.stopPropagation(); moved = false; } }, true);
    nav.addEventListener('click', ev => { ev.stopPropagation(); Object.assign(Z, { z: 1, x: 0, y: 0 }); apply(); });
  }

  // ---- curves (spectra) ----
  function curvePalette(light) {
    const edge = tok('panel-em-edge');
    return { text: tok('panel-curve-text'), lw: light ? 1.2 : 1.7, emFill: tok('panel-em-fill'), emEdge: /,\s*0\)$/.test(edge) ? null : edge,
      dichroic: tok('panel-dichroic'), exfilter: tok('panel-exfilter'), qe: tok('panel-qe'), laserW: light ? 2 : 3, laserEdge: tok('panel-laser-edge') };
  }
  function seriesStyle(s, P, light) {
    if (s.role === 'emfilter') return { fill: P.emFill, stroke: P.emEdge };
    if (s.role === 'ex' || s.role === 'em') return { stroke: waveColour(s.nm, light), dash: s.dash };
    return { stroke: P[s.role] || P.text, dash: s.dash };
  }
  // A curves view's frame on its canvas (device pixels, laid out on 300 x 110: k = W / 300): the plot's top and bottom,
  // the wavelengths' font, x and y in pixels.
  function curveFrame(canvas, v, big) {
    const W = canvas.width, H = canvas.height, k = W / 300, fs = (big ? 5 : 10) * k;
    const top = 8 * k, bot = H - (big ? 2 * fs + 9 * k : 16 * k);
    return { W, H, k, fs, top, bot, X: x => (x - v.x0) / (v.x1 - v.x0) * W, Y: y => bot - y * (bot - top) };
  }
  // A series' value at wavelength x (linear between its samples, which span x0 .. x1), or null outside it.
  function seriesAt(s, v, data, x) {
    if (s.len < 2) return null;
    const t = (x - v.x0) / (v.x1 - v.x0) * (s.len - 1);
    if (!(t >= 0 && t <= s.len - 1)) return null;
    const i = Math.min(s.len - 2, Math.floor(t)), f = t - i;
    return data[s.offset + i] * (1 - f) + data[s.offset + i + 1] * f;
  }
  // The legend entry at a point (px, py, device pixels) of a drawn curves view: the nearest curve or laser line within
  // r, else the emission filter's fill under the point; i -1 for none, with the wavelength and the curve's value there.
  function curveAt(canvas, v, data, big, px, py, r) {
    const F = curveFrame(canvas, v, big), ns = v.series.length, x = v.x0 + px / F.W * (v.x1 - v.x0);
    let i = -1, bd = r, val = 0, fill = -1, fillVal = 0;
    if (py < F.top - r || py > F.bot + r) return { i, x, val };
    (v.lines || []).forEach((l, j) => { const d = Math.abs(F.X(l.x) - px); if (d < bd) { bd = d; i = ns + j; } });
    v.series.forEach((s, j) => {
      const y = seriesAt(s, v, data, x);
      if (y === null) return;
      const d = Math.abs(F.Y(y) - py);
      if (d < bd) { bd = d; i = j; val = y; }
      if (s.fill && py >= F.Y(y) && py <= F.bot) { fill = j; fillVal = y; }
    });
    if (i < 0 && fill >= 0) { i = fill; val = fillVal; }
    return { i, x, val };
  }
  // On a canvas of W x H device pixels laid out on 300 x 110 (k = W / 300); returns the legend [label, style, role, i]
  // (i: the series' index, then the laser lines'). Filled (big), the view is drawn larger: the lines a quarter thinner,
  // the wavelengths half the size, an axis title. hl (a Set of legend indices, or null) marks entries: those drawn
  // thicker and on top, the rest faint.
  function drawCurves(canvas, v, data, light, big, hl) {
    const g = canvas.getContext('2d'), P = curvePalette(light);
    const { W, H, k, fs, top, bot, X, Y } = curveFrame(canvas, v, big);
    if (big) { P.lw *= 0.75; P.laserW *= 0.75; }
    g.clearRect(0, 0, W, H);
    g.font = fs + 'px sans-serif'; g.fillStyle = P.text;
    g.textAlign = 'center';
    const step = (v.x1 - v.x0) > 300 ? (big ? 50 : 100) : 50;
    for (let x = Math.ceil(v.x0 / step) * step; x <= v.x1; x += step) {
      const tx = Math.min(W - fs, Math.max(fs, X(x)));
      g.fillText(String(x), tx, big ? bot + 4 * k + fs : H - 3 * k);
      g.fillRect(X(x) - k / 2, bot, k, (big ? 2 : 3) * k);
    }
    if (big) g.fillText('wavelength (' + (v.unit || 'nm') + ')', W / 2, H - 2 * k);
    g.textAlign = 'start';
    const ns = v.series.length, lines = v.lines || [], legend = [];
    v.series.forEach((s, i) => legend.push([s.label, seriesStyle(s, P, light), s.role, i]));
    lines.forEach((l, j) => legend.push([l.label, { stroke: waveColour(l.nm, light), width: P.laserW, edge: P.laserEdge }, 'laser', ns + j]));
    const drawSeries = (s, st, on) => {
      const dx = (v.x1 - v.x0) / Math.max(1, s.len - 1), lw = P.lw * k * (on ? 2 : 1);
      const path = () => { g.beginPath(); for (let i = 0; i < s.len; i++) { const x = X(v.x0 + i * dx), y = Y(data[s.offset + i]); if (i) g.lineTo(x, y); else g.moveTo(x, y); } };
      if (s.fill) {
        path(); g.lineTo(X(v.x1), Y(0)); g.lineTo(X(v.x0), Y(0)); g.closePath(); g.fillStyle = st.fill; g.fill();
        if (st.stroke || on) { path(); g.setLineDash([]); g.strokeStyle = st.stroke || P.text; g.lineWidth = lw; g.stroke(); }
      } else {
        path(); g.setLineDash((st.dash || []).map(d => d * k)); g.strokeStyle = st.stroke; g.lineWidth = lw; g.stroke(); g.setLineDash([]);
      }
    };
    const drawLine = (l, on) => {
      // a laser line: its colour with an edge on both sides, so it stands apart from a dye curve of the same hue
      const c = waveColour(l.nm, light), x = X(l.x), w = P.laserW * k * (on ? 1.6 : 1), e = (light ? 1 : 1.5) * k;
      g.fillStyle = P.laserEdge; g.fillRect(x - w / 2 - e, top, w + 2 * e, bot - top);
      g.fillStyle = c; g.fillRect(x - w / 2, top, w, bot - top);
    };
    // the series, then the laser lines; marked ones last, over the others
    const marked = !!(hl && hl.size);
    for (const pass of [0, 1]) for (const [, st, , i] of legend) {
      const on = marked && hl.has(i);
      if (on !== (pass === 1)) continue;
      g.globalAlpha = marked && !on ? 0.18 : 1;
      if (i < ns) drawSeries(v.series[i], st, on); else drawLine(lines[i - ns], on);
    }
    g.globalAlpha = 1;
    canvas.dataset.lasers = lines.length;
    canvas.dataset.marked = marked ? [...hl].join(',') : '';
    return legend.map(([t, s, r, i]) => [t, Object.assign({ lw: P.lw }, s), r, i]);
  }
  // Links a filled curves view's plot and legend (in figure f): a pointer over a curve or a laser line marks it and its
  // legend entry and tells its value there; a pointer over a legend entry (a row's name: all of the row's; a dye's
  // name: its excitation and emission) marks its curves.
  function linkCurves(f, canvas, v, data, light, legend) {
    const L = canvas._link = Object.assign(canvas._link || { hl: null }, { v, data, light, legend });
    const note = f.querySelector('.pvNote');
    let tip = f.querySelector('.pTip');
    if (!tip) { tip = document.createElement('div'); tip.className = 'pTip'; tip.hidden = true; f.append(tip); }
    const mark = hl => {
      L.hl = hl && hl.size ? hl : null;
      drawCurves(canvas, L.v, L.data, L.light, true, L.hl);
      const box = note.querySelector('.pSpecLegend');
      if (!box) return;
      box.classList.toggle('lgHov', !!L.hl);
      for (const e of box.querySelectorAll('[data-i]')) e.classList.toggle('lgOn', !!L.hl && e.dataset.i.split(',').some(i => L.hl.has(+i)));
    };
    if (L.hl) mark(new Set([...L.hl].filter(i => i < L.legend.length)));
    if (canvas._linked) return;
    canvas._linked = true;
    const pct = x => (100 * x).toFixed(x < 0.1 ? 1 : 0) + ' %';
    canvas.addEventListener('pointermove', ev => {
      const r = canvas.getBoundingClientRect(), s = canvas.width / r.width;
      const hit = curveAt(canvas, L.v, L.data, true, (ev.clientX - r.left) * s, (ev.clientY - r.top) * s, 7 * s);
      mark(hit.i >= 0 ? new Set([hit.i]) : null);
      if (hit.i < 0) { tip.hidden = true; return; }
      const [label, , role] = L.legend.find(e => e[3] === hit.i);
      tip.textContent = role === 'laser' ? label
        : label + '\n' + Math.round(hit.x) + ' nm: ' + pct(hit.val) + (role === 'ex' || role === 'em' ? ' of its peak' : '');
      tip.hidden = false;
      const fr = f.getBoundingClientRect(), x = ev.clientX - fr.left, y = ev.clientY - fr.top, w = tip.offsetWidth;
      tip.style.left = Math.max(0, x + 14 + w > fr.width ? x - 14 - w : x + 14) + 'px';
      tip.style.top = (y + 14) + 'px';
    });
    canvas.addEventListener('pointerleave', () => { tip.hidden = true; mark(null); });
    note.addEventListener('pointerover', ev => {
      const e = ev.target.closest && ev.target.closest('.pSpecLegend [data-i]');
      mark(e ? new Set(e.dataset.i.split(',').map(Number)) : null);
    });
    note.addEventListener('pointerleave', () => mark(null));
  }
  // The legend in rows along the light path: the lasers, the excitation filter, the dichroic, the sample's dyes (each
  // its excitation and emission), the emission filter, the camera; a row's name left, its items after it.
  const LEGEND_ROWS = [['laser', 'Lasers'], ['exfilter', 'Excitation filter'], ['dichroic', 'Dichroic'], ['dye', 'Sample'],
    ['emfilter', 'Emission filter'], ['qe', 'Camera QE']];
  function legendHtml(legend) {
    const esc = t => String(t).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[c]);
    const sw = s => '<svg width="30" height="12" aria-hidden="true">' + (s.fill
      ? `<rect x="1" y="2" width="28" height="8" fill="${s.fill}"${s.stroke ? ` stroke="${s.stroke}"` : ''}/>`
      : (s.edge ? `<line x1="1" y1="6" x2="29" y2="6" stroke="${s.edge}" stroke-width="${(s.width || 2) + 3}"/>` : '') +
        `<line x1="1" y1="6" x2="29" y2="6" stroke="${s.stroke}" stroke-width="${s.width || s.lw + 0.3}"${s.dash && s.dash.length ? ` stroke-dasharray="${s.dash.join(' ')}"` : ''}/>`) + '</svg>';
    // every entry carries its legend index (data-i; a row's name and a dye's name all of theirs), for linkCurves
    const item = (s, t, i) => `<span data-i="${i}">${sw(s)}${esc(t)}</span>`;
    const rows = [];
    for (const [role, name] of LEGEND_ROWS) {
      let items = '';
      const all = [];
      if (role === 'dye') {
        // one line per dye: its excitation and emission (the labels "<dye> excitation" / "<dye> emission")
        const dyes = new Map();
        for (const [t, s, r, i] of legend) if (r === 'ex' || r === 'em') {
          const who = t.replace(/ (excitation|emission)$/, ''), d = dyes.get(who) || { html: [], i: [] };
          d.html.push(item(s, r === 'ex' ? 'excitation' : 'emission', i));
          d.i.push(i);
          all.push(i);
          dyes.set(who, d);
        }
        for (const [who, d] of dyes) items += `<span class="lgSub"><b data-i="${d.i.join(',')}">${esc(who)}</b>${d.html.join('')}</span>`;
      } else {
        // the part's name is the row's: an item says which one ("dichroic: Chroma ..." -> "Chroma ...")
        for (const [t, s, r, i] of legend) if (r === role) { items += item(s, role === 'laser' ? t : t.replace(/^[^:]*:\s*/, ''), i); all.push(i); }
      }
      if (items) rows.push(`<span class="lgHead" data-i="${all.join(',')}">${name}</span><span class="lgItems${role === 'dye' ? ' dyes' : ''}">${items}</span>`);
    }
    return rows.join('');
  }

  // ---- the drawn light path ----
  // One drawing of the light path, laid out along it: s runs from the sample (an inverted microscope's top, here the
  // start) to the camera, t across it. Wide boxes draw it lying down (the sample left, the camera right, the lasers
  // below the dichroic), narrow ones (a phone) standing up (the sample on top, the lasers right of the dichroic); the
  // second is the first mirrored about the diagonal, so the dichroic (a mirror at 45 degrees) reflects in both.
  // Excitation runs on one side of the axis (from the lasers through their filter to the dichroic, then to the
  // sample), each label's emission on the other (from the sample through the dichroic and the emission filter to the
  // camera), transmitted light on it. A beam is as opaque as it is strong ("weight") and after each part ("pass": the
  // fraction that part passes on) as much weaker; where under 2 % is left it ends at that part with a short bar across
  // it (blocked). Colours: the wavelength's (the laser line; the label's mean emission wavelength, as in the Spectra
  // panel). In a tile only the parts' names (a click opens this drawing filling the view); filled, each part's value, the
  // lasers' powers and a legend naming every beam, and a click on a part opens its station's panel.
  const SVGNS = 'http://www.w3.org/2000/svg';
  function el(tag, attrs, parent) {
    const e = document.createElementNS(SVGNS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.append(e);
    return e;
  }
  // design:allow -- the objectives' magnification colour code (ISO 8038 / DIN 58887), not the theme
  const MAG_COLOUR = [[100, '#f2f2f2'], [60, '#2d6fd6'], [40, '#7cc6f2'], [20, '#3fae4a'], [10, '#f2d03f'], [5, '#e08a2c'], [0, '#d9443a']];
  function clip(t, n) { t = String(t || ''); return t.length > n ? t.slice(0, Math.max(1, n - 1)) + '…' : t; }
  // the panels a part belongs to (data-for): its station and the one that also shows it
  const forOf = e => e.station + (e.also ? ' ' + e.also : '');

  // t in at most m lines of n characters (broken after a slash, at a space or a comma; the rest clipped)
  function wrapLines(t, n, m) {
    const out = [];
    t = String(t || '').trim();
    while (t && out.length < m - 1 && t.length > n) {
      let k = -1;
      for (let i = Math.min(n, t.length - 1); i > n / 3; i--) if (' ,/'.includes(t[i])) { k = i; break; }
      if (k < 0) break;
      out.push(t[k] === '/' ? t.slice(0, k + 1) : t.slice(0, k).replace(/,$/, ''));
      t = t.slice(k + 1).trim();
    }
    if (t) out.push(clip(t, n));
    return out;
  }
  function pct(f) { return f >= 0.995 ? '100 %' : f >= 0.1 ? (100 * f).toFixed(0) + ' %' : f >= 0.001 ? (100 * f).toFixed(1) + ' %' : '0 %'; }
  const MIN_SCALE = 0.75;   // the smallest scale the lying drawing is shown at (else it stands up)
  function drawSchematic(box, v, opts) {
    const light = !!opts.light, big = !!opts.big, detail = big;
    const ink = tok('panel-ink'), fillC = tok('panel-fill'), text = tok('panel-text'), muted = tok('panel-muted');
    const width = box.clientWidth || (box.parentElement && box.parentElement.clientWidth) || 800;
    box.textContent = '';
    box.classList.add('pSchem');
    const axis = v.elements.filter(e => e.side !== 'right'), side = v.elements.filter(e => e.side === 'right');
    const beams = k => v.beams.filter(b => b.kind === k);
    const exB = beams('excitation'), emB = beams('emission'), trB = beams('transmitted');
    for (const b of v.beams) b.stop = '';
    // the layout (units ~ CSS pixels at the drawing's usual width): lying down the parts are 84..140 apart to fill the
    // width; where even 84 would shrink the text below 75 % it stands up. Lying, the back focal plane sits just behind
    // the objective (BFP_GAP past its barrel's end) in a narrower slot (BFP_SLOT of a part's), its name below the axis
    // (the others' are above it); standing (little room along the axis), in a slot of its own like every part.
    const BFP_SLOT = 0.4, BFP_GAP = 14, slotH = e => e.kind === 'bfp' ? BFP_SLOT : 1;
    const horiz = width >= MIN_SCALE * (84 * axis.reduce((a, e) => a + slotH(e), 0) + 32);
    const slotOf = e => horiz ? slotH(e) : 1, n = axis.reduce((a, e) => a + slotOf(e), 0);
    const STEP = horiz ? Math.max(84, Math.min(140, Math.floor((width - 32) / n))) : 58, S0 = horiz ? 16 : 6;
    box.dataset.orient = horiz ? 'h' : 'v';
    const nameChars = horiz ? Math.floor(STEP / 7.2) : 30, valueChars = horiz ? Math.floor(STEP / 6.4) : 34;
    const AX = 300, AY = detail ? 118 : 62;                        // the axis' x (standing) or y (lying)
    const TEX = horiz ? (detail ? 64 : 52) : 170, TLAS = horiz ? (detail ? 112 : 88) : 260, BD = horiz ? 13 : 28;   // the excitation filter, the lasers' box (its half depth)
    const q = horiz ? 0.6 : 1;                      // lying down, the wide parts are narrower across (their names sit above them)
    const slots = [];   // each part's place along s: its slot's middle, the BFP's behind the objective's barrel
    axis.reduce((a, e, i) => {
      slots.push(horiz && e.kind === 'bfp' && i > 0 && axis[i - 1].kind === 'objective' ? S0 + STEP * (a - 0.5) + 22 + BFP_GAP : S0 + STEP * (a + slotOf(e) / 2));
      return a + slotOf(e);
    }, 0);
    const sAt = i => slots[i];
    const at = kind => { const i = axis.findIndex(e => e.kind === kind); return i < 0 ? null : sAt(i); };
    const sSample = at('sample'), sD = at('dichroic'), sEm = at('emfilter'), sCam = at('camera'), sLamp = at('lamp');
    // the arrows between the sample and the dichroic: in the gap behind the objective's barrel (before the BFP), clear of the parts
    const sObj = at('objective'), sBfp = at('bfp');
    // beam j's distance from the axis; the mirror's reach on each side of the axis: just past the beams that meet it
    // there (the lasers' above it, the emission below; the lamp's on the axis)
    const off = j => 9 + 7 * j, MUp = off(Math.max(0, exB.length - 1)) + 7, MDown = off(Math.max(0, emB.length - 1)) + 7;
    const sArrow = sBfp !== null && sD !== null ? (sBfp + sD - MUp) / 2 : sObj !== null && sD !== null ? (sObj + 22 + sD - MUp) / 2
                 : sSample !== null && sD !== null ? (sSample + sD) / 2 : 0;
    const laserLines = side.some(s => s.kind === 'lasers') ? String((side.find(s => s.kind === 'lasers') || {}).value || '').split(', ') : [];
    const W = horiz ? 2 * S0 + STEP * n : 640;
    const H = horiz ? AY + TLAS + BD + (detail ? 14 + 14 * Math.max(0, laserLines.length - 1) : 6) : 2 * S0 + STEP * n;
    const P = (s, t) => horiz ? [s, AY + t] : [AX + t, s];
    const pt = (s, t) => P(s, t).map(x => +x.toFixed(2)).join(' ');
    const line = pts => 'M' + pts.map(p => pt(p[0], p[1])).join(' L');
    const svg = el('svg', { viewBox: `0 0 ${W} ${H}`, class: 'pDraw', role: 'img', 'aria-label': 'The light path' });
    svg.style.maxWidth = Math.round(W * (big ? 1.5 : 1.1)) + 'px';
    const gHit = el('g', {}, svg), gBeam = el('g', {}, svg), gBeamHit = el('g', {}, svg), gPart = el('g', {}, svg), gText = el('g', {}, svg);
    // the beams
    const opac = w => (0.22 + 0.78 * Math.min(1, w)).toFixed(2);
    // the panels a beam belongs to (marked while the pointer is on it): a laser's: the illumination profile and the
    // spectra, an emission's: the spectra
    const BEAM_PANELS = { excitation: 'illumination spectra', emission: 'spectra' };
    const beam = (b, pts, w) => {
      const hit = el('path', { d: line(pts), stroke: 'transparent', 'stroke-width': 9, fill: 'none', class: 'pBeamHit' }, gBeamHit);
      hit.dataset.tip = beamInfo(b);
      hit.dataset.beam = v.beams.indexOf(b);
      if (BEAM_PANELS[b.kind]) hit.dataset.panels = BEAM_PANELS[b.kind];
      const p = el('path', { d: line(pts), stroke: waveColour(b.nm, light), 'stroke-width': b.kind === 'transmitted' ? 4 : 2.4,
        'stroke-opacity': opac(w), fill: 'none', 'stroke-linejoin': 'round', class: 'pBeamVis' }, gBeam);
      p.dataset.beam = hit.dataset.beam;
      return p;
    };
    // a bar across a beam that ends at (s, t), the beam running along s (alongS) or along t
    const blocked = (b, s, t, alongS) => el('path', { d: alongS ? line([[s, t - 5], [s, t + 5]]) : line([[s - 5, t], [s + 5, t]]),
      stroke: waveColour(b.nm, light), 'stroke-width': 2.4, 'stroke-opacity': 0.9 }, gBeam);
    // a small arrowhead on a beam at (s, t), pointing along s (dir +1: toward the camera, -1: toward the sample)
    const arrow = (b, s, t, dir, w) => el('path', { d: line([[s - 4 * dir, t - 4], [s + 4 * dir, t], [s - 4 * dir, t + 4]]) + ' Z',
      fill: waveColour(b.nm, light), 'fill-opacity': opac(w) }, gBeam);
    // the same along t (dir -1: toward the axis, from the lasers)
    const arrowT = (b, s, t, dir, w) => el('path', { d: line([[s - 4, t - 4 * dir], [s, t + 4 * dir], [s + 4, t - 4 * dir]]) + ' Z',
      fill: waveColour(b.nm, light), 'fill-opacity': opac(w) }, gBeam);
    const passOf = (b, kind) => { const p = (b.pass || []).find(r => r.at === kind); return p ? p.f : 1; };
    // a beam's pop-up: what it is and what each part passes on (a laser's power at the sample from the lasers' value)
    const beamInfo = b => {
      if (b.kind === 'excitation') {
        const at = laserLines.find(l => l.startsWith(Math.round(b.nm) + ' nm')), f1 = passOf(b, 'exfilter'), f2 = passOf(b, 'dichroic');
        return [b.label + (at ? ': ' + at.replace(/^\S+ nm (\(custom\) )?/, '') + ' at the sample' : ''),
          'the excitation filter passes ' + pct(f1) + ', the dichroic reflects ' + pct(f2),
          f1 * f2 < CUT ? 'blocked: it does not reach the sample' : pct(f1 * f2) + ' of the light set reaches the sample'].join('\n');
      }
      if (b.kind === 'emission') {
        const fd = passOf(b, 'dichroic'), fe = passOf(b, 'emfilter'), fq = passOf(b, 'camera');
        return [b.label + ' emission, mean ' + Math.round(b.nm) + ' nm', 'the dichroic passes ' + pct(fd) + ', the emission filter ' +
          pct(fe) + ', the camera detects ' + pct(fq), pct(fd * fe * fq) + ' of the emitted light is detected'].join('\n');
      }
      return b.label + ', ' + Math.round(b.nm) + ' nm';
    };
    const CUT = 0.02, sEnd = sCam !== null ? sCam - 13 : (horiz ? W : H) - 6;
    exB.forEach((b, j) => {
      if (sD === null || sSample === null) return;
      const o = off(j), s = sD - o, w0 = b.weight, f1 = passOf(b, 'exfilter'), f2 = passOf(b, 'dichroic');
      beam(b, [[s, TLAS - BD], [s, TEX]], w0);
      arrowT(b, s, (TLAS - BD + TEX) / 2, -1, w0);
      if (f1 < CUT) { blocked(b, s, TEX + 5, false); b.stop = 'exfilter'; return; }
      beam(b, [[s, TEX], [s, -o]], w0 * f1);
      arrowT(b, s, (TEX - o) / 2, -1, w0 * f1);
      // what the dichroic lets through (from 10 %: not every line's leak): a short stub on past the mirror, fading out
      const wt = w0 * f1 * (1 - f2);
      if (1 - f2 >= 0.1) {
        beam(b, [[s, -o], [s, -o - 9]], wt);
        el('path', { d: line([[s, -o - 9], [s, -o - 17]]), stroke: waveColour(b.nm, light), 'stroke-width': 2.4,
          'stroke-opacity': (0.4 * opac(wt)).toFixed(2), fill: 'none', class: 'pBeamVis' }, gBeam).dataset.beam = v.beams.indexOf(b);
      }
      if (f2 < CUT) { b.stop = 'dichroic'; return; }
      beam(b, [[s, -o], [sSample, -o]], w0 * f1 * f2);
      arrow(b, sArrow, -o, -1, w0 * f1 * f2);
    });
    emB.forEach((b, j) => {
      if (sSample === null) return;
      const t = off(j), fd = passOf(b, 'dichroic'), fe = passOf(b, 'emfilter');
      if (sD === null) { beam(b, [[sSample, t], [sEnd, t]], b.weight); return; }
      beam(b, [[sSample, t], [sD, t]], b.weight);
      arrow(b, sArrow, t, 1, b.weight);
      if (fd < CUT) { blocked(b, sD + 4, t, true); b.stop = 'dichroic'; return; }
      beam(b, [[sD, t], [sEm !== null ? sEm : sEnd, t]], b.weight * fd);
      if (sEm === null) return;
      if (fe < CUT) { blocked(b, sEm - 5, t, true); b.stop = 'emfilter'; return; }
      beam(b, [[sEm, t], [sEnd, t]], b.weight * fd * fe);
    });
    trB.forEach(b => { if (sLamp !== null) beam(b, [[sLamp + 10, 0], [sEnd, 0]], b.weight); });
    // the parts, each drawn around its place s0 on the axis (ds along the light, dt across it)
    const sh = { fill: fillC, stroke: ink, 'stroke-width': 1.2 };
    const shape = (g, s0, pts, attrs) => el('path', Object.assign({ d: line(pts.map(p => [s0 + p[0], p[1]])) + ' Z' }, attrs), g);
    const box4 = (g, s0, a, b, c, d, attrs) => shape(g, s0, [[a, c], [b, c], [b, d], [a, d]], attrs);   // ds a..b, dt c..d
    axis.forEach((e, i) => {
      const st = detail ? e.station : '', s0 = sAt(i), g = el('g', { class: 'pPart' + (st ? ' pClick' : '') }, gPart);
      if (st) g.dataset.station = st;
      if (e.station) g.dataset.for = forOf(e);
      g.dataset.kind = e.kind;
      // the whole slot answers a click (its name included)
      const half = STEP * slotOf(e) / 2, bfp = horiz && e.kind === 'bfp';
      const hit = horiz ? el('rect', bfp ? { x: s0 - half, y: AY - 44, width: 2 * half, height: H - AY + 44, fill: 'transparent' }
                                         : { x: s0 - half, y: 0, width: 2 * half, height: AY + 44, fill: 'transparent' }, gHit)
                        : el('rect', { x: 0, y: s0 - half, width: AX + 70, height: 2 * half, fill: 'transparent' }, gHit);
      if (st) { hit.dataset.station = st; hit.setAttribute('class', 'pClick'); }
      if (e.kind === 'lamp') {
        const [cx, cy] = P(s0 - 2, 0);
        el('circle', Object.assign({ cx, cy, r: 9 }, sh), g);
        for (let a = 0; a < 8; a++) { const r = a * Math.PI / 4; el('line', { x1: cx + 12 * Math.cos(r), y1: cy + 12 * Math.sin(r), x2: cx + 16 * Math.cos(r), y2: cy + 16 * Math.sin(r), stroke: ink, 'stroke-width': 1.2 }, g); }
      } else if (e.kind === 'sample') {
        box4(g, s0, -12, -2, -62 * q, 62 * q, { fill: tok('panel-sample-fill'), stroke: tok('panel-sample-stroke') });
        box4(g, s0, -2, 2, -72 * q, 72 * q, { fill: tok('panel-glass'), stroke: ink, 'stroke-width': 0.8 });   // the coverslip
      } else if (e.kind === 'objective') {
        // an objective pointing at the sample: the front lens, the nose, the barrel with its magnification's colour ring
        const mag = parseFloat(e.label) || 100, ring = MAG_COLOUR.find(m => mag >= m[0])[1];
        shape(g, s0, [[-19, -13], [-19, 13], [-8, 24], [-8, -24]], sh);
        box4(g, s0, -8, 22, -26, 26, sh);
        box4(g, s0, -3, 1, -26, 26, { fill: ring, stroke: ink, 'stroke-width': 0.6 });
        for (let k = -20; k <= 20; k += 5) el('path', { d: line([[s0 + 12, k], [s0 + 20, k]]), stroke: ink, 'stroke-width': 0.7, 'stroke-opacity': 0.7 }, g);
        const [ax, ay] = P(s0 - 19, -10), [bx, by] = P(s0 - 25, 0), [cx2, cy2] = P(s0 - 19, 10);
        el('path', { d: `M${ax} ${ay} Q${bx} ${by} ${cx2} ${cy2}`, fill: tok('panel-lens'), stroke: ink, 'stroke-width': 0.8 }, g);
      } else if (e.kind === 'bfp') {
        el('path', { d: line([[s0, -58 * q], [s0, 58 * q]]), stroke: ink, 'stroke-width': 1.2, 'stroke-dasharray': '4 3' }, g);
      } else if (e.kind === 'dichroic') {
        el('path', { d: line([[s0 - MUp, -MUp], [s0 + MDown, MDown]]), stroke: tok('panel-mirror'), 'stroke-width': 4, 'stroke-linecap': 'round' }, g);
      } else if (e.kind === 'emfilter') {
        box4(g, s0, -4, 4, -40 * q, 40 * q, { fill: ink, 'fill-opacity': 0.75 });
      } else if (e.kind === 'camera') {
        box4(g, s0, -13, 17, -34, 34, sh);
        box4(g, s0, -13, -9, -20, 20, { fill: ink, 'fill-opacity': 0.6 });   // the sensor
      } else box4(g, s0, -10, 10, -34, 34, sh);
      // its name and value: left of it standing, above it lying
      // the name (up to two lines) and the value (up to two): lying, every name ends on one line and every value
      // starts under it, above the parts; standing, centred on the part, left of it
      const below = horiz && e.kind === 'bfp';   // across the axis: the back focal plane, close behind the objective
      const tx = horiz ? s0 : AX - 84, anchor = horiz ? 'middle' : 'end';
      const names = horiz ? wrapLines(e.label, nameChars, 2) : [clip(e.label, nameChars)], vals = detail && e.value ? wrapLines(e.value, valueChars, horiz ? 3 : 2) : [];
      // the name's last line: lying, above the parts (the BFP: its first line under the dashed line)
      const yName = horiz ? (below ? AY + 58 * q + 14 + 14 * (names.length - 1) : AY - (detail ? 78 : 42)) : s0 + 4 - 6.5 * vals.length;
      const t = el('text', { x: tx, y: yName, 'text-anchor': anchor, fill: text, 'font-size': horiz ? 12 : 13, 'font-weight': 600 }, gText);
      names.forEach((l, k) => {
        const ts = el('tspan', { x: tx, dy: k ? 14 : -14 * (names.length - 1) }, t);
        ts.textContent = l;
      });
      vals.forEach((l, k) => {
        const s = el('text', { x: tx, y: yName + 13 * (k + 1), 'text-anchor': anchor, fill: muted, 'font-size': 11 }, gText);
        s.textContent = l;
      });
      g.dataset.tip = hit.dataset.tip = t.dataset.tip = e.label + (e.value ? ': ' + e.value : '') + (e.note ? '\n' + e.note : '');
      g.dataset.part = hit.dataset.part = t.dataset.part = e.kind;
      if (st) { t.dataset.station = st; t.setAttribute('class', 'pClick'); }
      if (e.station) t.dataset.for = forOf(e);
    });
    // the side parts: the lasers (one dot per line, on its beam) and their filter
    if (sD !== null) side.forEach(s => {
      const st = detail ? s.station : '', g = el('g', { class: 'pPart' + (st ? ' pClick' : '') }, gPart);
      if (st) g.dataset.station = st;
      if (s.station) g.dataset.for = forOf(s);
      g.dataset.kind = s.kind;
      const a = Math.min(sD - off(Math.max(0, exB.length - 1)) - 9, sD - 21), b = Math.max(sD - off(0) + 9, a + 24);   // the beams' span along s
      if (s.kind === 'lasers') {
        box4(g, 0, a, b, TLAS - BD, TLAS + BD, sh);
        exB.forEach((bm, j) => { const [cx, cy] = P(sD - off(j), TLAS - BD / 2); el('circle', { cx, cy, r: 3.2, fill: waveColour(bm.nm, light) }, g); });
        // its name and the lines: right of the box lying, above and below it standing
        const [nx, ny] = horiz ? P(b + 10, TLAS - 2) : P(a - 8, TLAS);
        const nm = el('text', { x: nx, y: ny, 'text-anchor': horiz ? 'start' : 'middle', fill: text, 'font-size': 12, 'font-weight': 600 }, gText);
        nm.textContent = s.label;
        nm.dataset.part = s.kind;
        nm.dataset.tip = s.label + (s.value ? ': ' + s.value : '');
        if (s.station) nm.dataset.for = forOf(s);
        if (detail) laserLines.forEach((l, k) => {
          const [lx, ly] = horiz ? P(b + 10, TLAS + 12 + 14 * k) : P(b + 15 + 13 * k, TLAS);
          const tl = el('text', { x: lx, y: ly, 'text-anchor': horiz ? 'start' : 'middle', fill: k < exB.length ? waveColour(exB[k].nm, light) : muted, 'font-size': 11 }, gText);
          tl.textContent = clip(l, 22);
        });
        if (horiz) el('rect', { x: a, y: AY + TLAS - BD - 4, width: (b - a) + 150, height: 2 * BD + 14 * laserLines.length + 8, fill: 'transparent' }, g);
        else el('rect', { x: AX + TLAS - 40, y: a - 24, width: 80, height: (b - a) + 30 + 13 * laserLines.length, fill: 'transparent' }, g);
      } else if (s.kind === 'exfilter') {
        box4(g, 0, a - 2, b + 2, TEX - 4, TEX + 4, { fill: ink, 'fill-opacity': 0.75 });
        const [fx, fy] = horiz ? P(b + 10, TEX - 2) : P(a - 23, TEX);
        const t = el('text', { x: fx, y: fy, 'text-anchor': horiz ? 'start' : 'middle', fill: text, 'font-size': 11.5, 'font-weight': 600 }, gText);
        t.textContent = s.label;
        t.dataset.part = s.kind;
        t.dataset.tip = s.label + (s.value ? ': ' + s.value : '');
        if (s.station) t.dataset.for = forOf(s);
        if (detail) {
        const t2 = el('text', { x: fx, y: fy + 13, 'text-anchor': horiz ? 'start' : 'middle', fill: muted, 'font-size': 11 }, gText);
        t2.textContent = clip(s.value, horiz ? Math.max(16, Math.floor((W - b - 14) / 6.2)) : 24);
        }
        if (horiz) el('rect', { x: a - 4, y: AY + TEX - 10, width: (b - a) + 160, height: 20, fill: 'transparent' }, g);
        else el('rect', { x: AX + TEX - 26, y: a - 20, width: 52, height: (b - a) + 26, fill: 'transparent' }, g);
      }
      g.dataset.tip = s.label + (s.value ? ': ' + s.value : '');
      g.dataset.part = s.kind;
    });
    box.append(svg);
    // the pop-up: a part's or a beam's info (data-tip) next to the pointer, at once
    const tip = document.createElement('div');
    tip.className = 'pTip';
    tip.hidden = true;
    box.append(tip);
    let hovKey = '';
    const hover = t => {
      const key = !t ? '' : t.dataset.beam !== undefined ? 'b' + t.dataset.beam : t.dataset.part ? 'p' + t.dataset.part : '';
      if (key === hovKey) return;
      hovKey = key;
      for (const e of svg.querySelectorAll('.pHov')) e.classList.remove('pHov');
      if (key[0] === 'b') for (const e of svg.querySelectorAll(`.pBeamVis[data-beam="${key.slice(1)}"]`)) e.classList.add('pHov');
      if (key[0] === 'p') for (const e of svg.querySelectorAll(`g[data-part="${key.slice(1)}"], text[data-part="${key.slice(1)}"]`)) e.classList.add('pHov');
      // the panels it belongs to: a part's station and also (data-for on its shapes), a beam's
      let panels = '';
      if (key[0] === 'b') panels = t.dataset.panels || '';
      if (key[0] === 'p') { const f = svg.querySelector(`g[data-part="${key.slice(1)}"]`); panels = (f && f.dataset.for) || ''; }
      box.dispatchEvent(new CustomEvent('isc-part-hover', { bubbles: true, detail: { panels: panels ? panels.split(' ') : [] } }));
    };
    svg.addEventListener('pointermove', ev => {
      const t = ev.target.closest && ev.target.closest('[data-tip]');
      hover(t && t.dataset.tip ? t : null);
      if (!t || !t.dataset.tip) { tip.hidden = true; return; }
      tip.textContent = t.dataset.tip;
      tip.hidden = false;
      const r = box.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
      // right of and below the pointer, kept inside the box (left of it near the right edge)
      const w = tip.offsetWidth, h = tip.offsetHeight;
      tip.style.left = Math.max(0, x + 14 + w > r.width ? x - 14 - w : x + 14) + 'px';
      tip.style.top = Math.max(0, Math.min(r.height - h, y + 16)) + 'px';
    });
    svg.addEventListener('pointerleave', () => { tip.hidden = true; hover(null); });
    box.dataset.detail = detail ? '1' : '';
    // the legend: each beam, its colour, its way, how much of it arrives
    const lg = document.createElement('div');
    lg.className = 'pLegend pBeamLegend';
    const esc = t => String(t).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
    const sw = b => `<svg width="22" height="10" aria-hidden="true"><line x1="1" y1="5" x2="21" y2="5" stroke="${waveColour(b.nm, light)}" stroke-width="${b.kind === 'transmitted' ? 4 : 2.6}"/></svg>`;
    const NAMES = { exfilter: 'the excitation filter', dichroic: 'the dichroic', emfilter: 'the emission filter' };
    const items = [];
    for (const b of exB) items.push(`<span>${sw(b)}${esc(b.label)}${b.stop ? ': blocked by ' + NAMES[b.stop] : ', ' + pct(passOf(b, 'exfilter') * passOf(b, 'dichroic')) + ' to the sample'}</span>`);
    for (const b of emB) items.push(`<span title="mean emission ${Math.round(b.nm)} nm">${sw(b)}${esc(b.label)} emission${b.stop ? ': blocked by ' + NAMES[b.stop] : ', ' + pct(passOf(b, 'dichroic') * passOf(b, 'emfilter')) + ' through the filters'}</span>`);
    for (const b of trB) items.push(`<span>${sw(b)}${esc(b.label)}, ${Math.round(b.nm)} nm</span>`);
    if (!items.length) items.push('<span>no light: the lasers off or the shutter closed</span>');
    lg.innerHTML = items.join('');
    if (detail) box.append(lg);
    box.dataset.elements = v.elements.length;
    box.dataset.beams = v.beams.length;
    box.dataset.lasers = exB.length;
    box.dataset.blocked = v.beams.filter(b => b.stop).map(b => b.kind + '@' + b.stop).join(',');
  }

  // ---- a panel ----
  // Draws content ({header, data}) into host: the title, the views, the controls and (filling the view) the caption,
  // the views' titles and notes, rows and legends. Rebuilt only when the panel's shape changes (another panel, other
  // views or controls), so a slider being dragged stays. opts: {big, light, title (the tile's), onParam(name, value),
  // onOpen (a click on the tile, or a station's id), onClose}.
  function shapeOf(h) { return h.id + '|' + h.views.map(v => v.type + ':' + v.name).join(',') + '|' + h.controls.map(c => c.name).join(','); }
  function decimals(step) { const s = String(step); return s.includes('.') ? s.split('.')[1].length : 0; }
  function drawPanel(host, content, opts) {
    opts = opts || {};
    const h = content.header, data = content.data, big = !!opts.big;
    host._pLast = [content, opts];   // (redrawPanel: the same content at a new size)
    const shape = shapeOf(h) + (big ? '|big' : '');
    if (host._pShape !== shape) {
      host._pShape = shape;
      host.textContent = '';
      host.className = 'pnl' + (big ? ' big' : '');
      host.innerHTML = '<div class="pnlHead"><span class="pnlTitle"></span><button class="pnlBtn"></button></div><div class="pnlViews"></div><div class="pnlCtl"></div><div class="pnlCap"></div>';
      const btn = host.querySelector('.pnlBtn');
      btn.textContent = big ? '✕' : '⤢';
      btn.title = big ? 'Back to every panel (Esc)' : 'Fill the view with this panel';
      btn.addEventListener('click', e => { e.stopPropagation(); if (big) { if (opts.onClose) opts.onClose(); } else if (opts.onOpen) opts.onOpen(h.id); });
      const views = host.querySelector('.pnlViews'), links = {};   // the images zoomed together, per link
      if (h.views.every(v => v.type === 'image')) views.classList.add('imgs');
      // a grid of h.columns columns: their widths in the first row's aspects, the rows sharing the view's height
      if (h.columns > 0) {
        const asp = h.views.slice(0, h.columns).map(v => v.extent ? v.extent[0] / v.extent[1] : v.w && v.h ? v.w / v.h : 1);
        views.classList.add('grid');
        views.style.gridTemplateColumns = asp.map(a => a.toFixed(4) + 'fr').join(' ');
        views.style.setProperty('--rows', Math.ceil(h.views.length / h.columns));
        views.style.setProperty('--aRow', asp.reduce((x, y) => x + y, 0).toFixed(4));
        views.style.setProperty('--cols', h.columns);
      }
      for (const v of h.views) {
        const f = document.createElement('figure');
        f.className = 'pv pv-' + v.type;
        f.dataset.name = v.name;
        f.innerHTML = '<figcaption></figcaption>' + (v.type === 'image' ? '<div class="pvImg"><canvas></canvas><div class="pvNav" hidden title="The zoom (the wheel over the image) and the part shown; a drag pans it. Click: the whole image"><i></i></div></div>'
          : v.type === 'curves' ? '<canvas></canvas>' : '<div class="pvBody"></div>') + '<div class="pvNote"></div>';
        if (v.type === 'image') {
          const a = v.extent ? v.extent[0] / v.extent[1] : v.w / v.h;
          f.style.setProperty('--a', a.toFixed(4));   // (a tile caps the image's height: the CSS's max-width)
          const c = f.querySelector('canvas');
          c.style.aspectRatio = `${a.toFixed(4)}`;
          if (v.pixelated) c.style.imageRendering = 'pixelated';
          attachZoom(c, v, v.link ? (links[v.link] = links[v.link] || []) : null);
        }
        views.append(f);
      }
      const ctl = host.querySelector('.pnlCtl');
      for (const c of h.controls) {
        const r = document.createElement('label');
        r.className = 'pc';
        r.title = c.note || '';
        r.innerHTML = '<span class="pcL"></span><input type="range" autocomplete="off"><span class="pcV"></span>';
        r.querySelector('.pcL').textContent = c.label;
        const inp = r.querySelector('input'), val = r.querySelector('.pcV');
        inp.dataset.name = c.name;
        inp.addEventListener('input', () => {
          val.textContent = (+inp.value).toFixed(decimals(inp.step)) + (c.unit ? ' ' + c.unit : '');
          if (opts.onParam) opts.onParam(c.name, +inp.value);
        });
        val.addEventListener('dblclick', () => { if (opts.onParam) opts.onParam(c.name, c.reset); });
        ctl.append(r);
      }
      host.addEventListener('click', e => {
        const st = e.target.closest('[data-station]');
        if (st && st.dataset.station) { if (opts.onOpen) opts.onOpen(st.dataset.station); return; }
        if (!big && !e.target.closest('.pnlCtl,button,input') && opts.onOpen) opts.onOpen(h.id);
      });
    }
    host.querySelector('.pnlTitle').textContent = big ? h.title : (opts.title || h.title);
    host.querySelector('.pnlCap').textContent = big ? h.caption : '';
    host.dataset.panel = h.id;
    host.dataset.views = h.views.length;
    // the controls' ranges and values (one being dragged keeps the value under the pointer)
    for (const c of h.controls) {
      const inp = host.querySelector(`.pnlCtl input[data-name="${c.name}"]`);
      if (!inp) continue;
      inp.min = c.min; inp.max = c.max; inp.step = c.step;
      if (document.activeElement !== inp) inp.value = c.value;
      inp.nextElementSibling.textContent = (+inp.value).toFixed(decimals(c.step)) + (c.unit ? ' ' + c.unit : '');
    }
    const peaks = {};
    for (const v of h.views) if (v.type === 'image') peaks[v.name] = peakOf(data, v);
    for (const v of h.views) {
      const f = host.querySelector(`.pv[data-name="${v.name}"]`);
      if (!f) continue;
      f.querySelector('figcaption').textContent = big ? (v.title || '') : '';
      f.querySelector('.pvNote').textContent = big ? (v.note || '') : '';
      if (v.type === 'image') drawImage(f.querySelector('canvas'), v, data, v.peakOf && peaks[v.peakOf] ? peaks[v.peakOf] : v.peak > 0 ? v.peak : peaks[v.name]);
      else if (v.type === 'curves') {
        const c = f.querySelector('canvas'), k = big ? 4 : 2;
        c.width = 300 * k; c.height = 110 * k;
        const legend = drawCurves(c, v, data, !!opts.light, big, big && c._link ? c._link.hl : null);
        f.querySelector('.pvNote').innerHTML = big ? '<div class="pvHead">Legend</div><div class="pLegend pSpecLegend">' + legendHtml(legend) + '</div>' : '';
        if (big) linkCurves(f, c, v, data, !!opts.light, legend);
      } else if (v.type === 'rows') {
        const b = f.querySelector('.pvBody');
        b.textContent = '';
        for (const [l, val] of v.rows) {
          // two cells: its name (none: it goes on with the row above) and its text, in the legend's grid and font
          const n = document.createElement('span'), t = document.createElement('span');
          n.className = 'rwL';
          t.className = 'rwV';
          n.textContent = l || '';
          t.textContent = val;
          b.append(n, t);
        }
      } else if (v.type === 'schematic') drawSchematic(f.querySelector('.pvBody'), v, opts);
    }
  }

  // Marks the parts of a drawn light path (in host) that belong to panel id ('' : none): a pointer over a panel shows
  // where in the light path it is.
  function highlightParts(host, id) {
    if (!host) return;
    for (const e of host.querySelectorAll('.pDraw [data-for]')) e.classList.toggle('pHl', !!id && e.dataset.for.split(' ').includes(id));
  }

  // Draws a panel again from what it last drew (a new width can turn the light path's drawing).
  function redrawPanel(host) { if (host && host._pLast) drawPanel(host, host._pLast[0], host._pLast[1]); }

  const api = { readPanel, readPanelList, drawPanel, redrawPanel, highlightParts, waveColour };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  root.ISC_SCOPE_PANELS = api;
})(typeof self !== 'undefined' ? self : globalThis);
