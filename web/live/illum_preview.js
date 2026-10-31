// The illumination preview (2026-10-30, spec/ALGORITHM.md "Structured illumination"): the light entering the
// objective's back focal plane seen from above, and the pattern it makes in the sample seen from below (the focal
// plane) and from the side (an x-z section through the axis), for one frame of the frame program. Page-free: the
// viewer's Preview illumination dialog (web/index.html) and the docs' interactive figure (web/illumination.html)
// both draw with it. The data comes from the module's isc_illum_preview (cli/scope_wasm.cpp; readIllumPreview below
// unpacks it): images n x n (bfp, xy), nz x n (xz), the beams (9 numbers each: ux, uy, spot radius in u, amplitude,
// phase in turns, polarization angle in degrees, u, evanescent, ellipticity in turns). The section seen from below
// is at a height (the focus by default: a slider moves it); the captions are text on the page, not on the canvases.
(function (root) {
  'use strict';

  // Calls the module's isc_illum_preview for spec at frame (the sizing call, then the filling call) and unpacks it.
  // zUm: the height of the section seen from below (undefined or < 0: the focus); the result's zUm is the height used.
  function readIllumPreview(M, spec, frame, n, nz, zUm) {
    n = n || 160; nz = nz || 96;
    zUm = zUm >= 0 ? zUm : -1;
    const len = M.lengthBytesUTF8(spec) + 1, s = M._malloc(len), info = M._malloc(12 * 8), err = M._malloc(512);
    M.stringToUTF8(spec, s, len);
    let out = 0, res;
    try {
      if (!M._isc_illum_preview) return { error: 'this module has no illumination preview (regenerate web/insiliscope_module.js)' };
      const need = M._isc_illum_preview(s, frame, zUm, n, nz, 0, 0, info, err, 512);
      if (need < 0) return { error: M.UTF8ToString(err) };
      out = M._malloc(Math.max(4, need * 4));
      if (M._isc_illum_preview(s, frame, zUm, n, nz, out, need, info, err, 512) < 0) return { error: M.UTF8ToString(err) };
      const v = M.HEAPF64.slice(info / 8, info / 8 + 12);
      const N = v[0], NZ = v[1], nb = v[6];
      const f = M.HEAPF32.slice(out / 4, out / 4 + need);
      let o = 0;
      const bfp = f.slice(o, o += N * N), xy = f.slice(o, o += N * N), xz = f.slice(o, o += NZ * N);
      const beams = [];
      for (let k = 0; k < nb; k++) {
        const b = f.slice(o, o += 9);
        beams.push({ ux: b[0], uy: b[1], spotU: b[2], amp: b[3], phaseTurns: b[4], polDeg: b[5], u: b[6], evanescent: b[7] > 0.5, ellipTurns: b[8] });
      }
      res = { n: N, nz: NZ, xySpanUm: v[2], zMaxUm: v[3], uSpan: v[4], uMax: v[5], period: v[7], angle: v[8], phase: v[9],
        patterned: v[10] > 0.5, zUm: v[11], frame, bfp, xy, xz, beams };
    } finally {
      for (const p of [s, info, err, out]) if (p) M._free(p);
    }
    return res;
  }

  // A heat image of data (n x n, or rows x n) on a canvas, each scaled to peak (or to `peak`), a colour map from
  // black through the hue to white; the canvas is resized to the data.
  const RAMP = (() => {
    const r = new Uint8Array(256 * 3);
    for (let i = 0; i < 256; i++) {
      const t = i / 255;
      // dark blue-black -> orange -> white
      r[3 * i] = Math.round(255 * Math.min(1, Math.max(0, 1.6 * t)));
      r[3 * i + 1] = Math.round(255 * Math.min(1, Math.max(0, 1.6 * t - 0.45)));
      r[3 * i + 2] = Math.round(255 * Math.min(1, Math.max(0, t < 0.25 ? 1.2 * t + 0.1 : 2.2 * t - 1.2)));
    }
    return r;
  })();
  function heat(canvas, data, w, h, peak, flipY) {
    canvas.width = w; canvas.height = h;
    const g = canvas.getContext('2d'), img = g.createImageData(w, h), px = img.data;
    let max = peak || 0;
    if (!peak) for (let i = 0; i < data.length; i++) if (data[i] > max) max = data[i];
    const k = max > 0 ? 255 / max : 0;
    for (let y = 0; y < h; y++) {
      const sy = flipY ? h - 1 - y : y;
      for (let x = 0; x < w; x++) {
        const v = Math.max(0, Math.min(255, Math.round(data[sy * w + x] * k)));
        const o = 4 * (y * w + x);
        px[o] = RAMP[3 * v]; px[o + 1] = RAMP[3 * v + 1]; px[o + 2] = RAMP[3 * v + 2]; px[o + 3] = 255;
      }
    }
    g.putImageData(img, 0, 0);
    return max;
  }

  // The three views on their canvases (any may be null): the pupil with the critical circle (u = 1), the pupil edge
  // (u = uMax) and each beam's spot ringed; the section at d.zUm seen from below; the x-z section with the coverslip
  // at the bottom and the section's height dashed. No text on the canvases: the captions are illumPreviewLabels.
  // style: {line} colour. Returns the peaks.
  function drawIllumPreview(d, canvases, style) {
    style = style || {};
    const line = style.line || 'rgba(255,255,255,0.85)';
    const out = {};
    if (canvases.bfp) {
      const c = canvases.bfp, n = d.n;
      out.bfp = heat(c, d.bfp, n, n, 0, true);
      const g = c.getContext('2d'), s = n / (2 * d.uSpan);
      const U = (u, v) => [n / 2 + u * s, n / 2 - v * s];
      g.lineWidth = 1;
      g.strokeStyle = line;
      g.setLineDash([3, 3]);
      g.beginPath(); g.arc(n / 2, n / 2, 1.0 * s, 0, 2 * Math.PI); g.stroke();   // the critical angle
      g.setLineDash([]);
      g.beginPath(); g.arc(n / 2, n / 2, d.uMax * s, 0, 2 * Math.PI); g.stroke();   // the pupil edge
      for (const b of d.beams) {
        const [x, y] = U(b.ux, b.uy);
        g.strokeStyle = b.evanescent ? '#7cf' : line;
        g.beginPath(); g.arc(x, y, Math.max(3, b.spotU * s), 0, 2 * Math.PI); g.stroke();
        // the polarization: a bar across the plane of incidence (s) or along it (p), by the angle; a circular beam
        // (an ellipticity of a quarter turn) a circular arrow around its spot
        const phi = Math.atan2(b.uy, b.ux), a = (b.polDeg < -180 ? 45 : b.polDeg) * Math.PI / 180;
        const L = Math.max(5, b.spotU * s + 4);
        if (Math.abs(Math.abs(b.ellipTurns || 0) - 0.25) < 0.05) {
          const r = L + 2, a0 = -Math.PI / 2, a1 = a0 + 1.6 * Math.PI, hx = x + r * Math.cos(a1), hy = y + r * Math.sin(a1);
          g.beginPath(); g.arc(x, y, r, a0, a1); g.stroke();
          const t = a1 + Math.PI / 2, h = 4;   // the arrowhead along the arc's direction at its end
          g.beginPath(); g.moveTo(hx, hy); g.lineTo(hx - h * Math.cos(t) + h * 0.6 * Math.sin(t), hy - h * Math.sin(t) - h * 0.6 * Math.cos(t));
          g.moveTo(hx, hy); g.lineTo(hx - h * Math.cos(t) - h * 0.6 * Math.sin(t), hy - h * Math.sin(t) + h * 0.6 * Math.cos(t)); g.stroke();
        } else {
          const dir = phi + Math.PI / 2 - a;   // s is across the azimuth; p along it
          g.beginPath(); g.moveTo(x - L * Math.cos(dir), y + L * Math.sin(dir)); g.lineTo(x + L * Math.cos(dir), y - L * Math.sin(dir)); g.stroke();
        }
      }
    }
    if (canvases.xy) {
      const c = canvases.xy, n = d.n;
      out.xy = heat(c, d.xy, n, n, 0, true);
      const g = c.getContext('2d');
      g.strokeStyle = line; g.lineWidth = 1;
      const f = n / 1.5, o = (n - f) / 2;   // the FOV inside the 1.5 x span
      g.strokeRect(o, o, f, f);
    }
    if (canvases.xz) {
      const c = canvases.xz, n = d.n, nz = d.nz;
      out.xz = heat(c, d.xz, n, nz, out.xy || 0, true);
      const g = c.getContext('2d');
      g.strokeStyle = line; g.lineWidth = 1;
      g.beginPath(); g.moveTo(0, nz - 0.5); g.lineTo(n, nz - 0.5); g.stroke();   // the coverslip
      if (d.zMaxUm > 0 && d.zUm >= 0 && d.zUm <= d.zMaxUm) {   // the section's height
        const yz = nz - 0.5 - d.zUm / d.zMaxUm * nz;
        g.setLineDash([3, 3]);
        g.beginPath(); g.moveTo(0, yz); g.lineTo(n, yz); g.stroke();
        g.setLineDash([]);
      }
    }
    return out;
  }

  // The captions of the three views, {title, note} each, for the page to show above and under the canvases.
  function illumPreviewLabels(d) {
    return {
      bfp: { title: 'Back focal plane, from above',
        note: 'the critical circle u = 1 dashed, the pupil edge solid; each beam\'s spot ringed, its polarization a bar (circular: an arrow); evanescent beams in blue' },
      xy: { title: 'Section at z = ' + d.zUm.toFixed(2) + ' µm, from below',
        note: 'the excitation over 1.5× the field (the field framed)' },
      xz: { title: 'Side, x-z through the axis',
        note: 'the coverslip at the bottom, ' + d.zMaxUm + ' µm up; the dashed line is the section\'s height' },
    };
  }

  // One line on the state: "frame 3 of 15: angle 0, phase 3" or "one beam".
  function illumPreviewCaption(d) {
    if (!d.patterned) return d.beams.length === 1 ? 'one beam at u = ' + d.beams[0].u.toFixed(2) : d.beams.length + ' beams';
    return 'frame ' + (((d.frame % d.period) + d.period) % d.period) + ' of ' + d.period + ': angle ' + d.angle + ', phase ' + d.phase +
      ' (' + d.beams.length + ' beams)';
  }

  const api = { readIllumPreview, drawIllumPreview, illumPreviewCaption, illumPreviewLabels };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  root.ISC_ILLUM_PREVIEW = api;
})(typeof self !== 'undefined' ? self : globalThis);
