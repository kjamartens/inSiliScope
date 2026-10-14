#!/usr/bin/env node
// Copyright (c) 2026, inSiliScope contributors. BSD-3-Clause (see LICENSE).
//
// Fetches webSMLM (https://github.com/HohlbeinLab/webSMLM, MIT, one self-contained HTML file) into
// web/websmlm/webSMLM.html and adds one <script> tag, web/live/websmlm_bridge.js, so the viewer site serves a webSMLM
// that takes the live microscope's frames: open it in any tab, and the live panel's "⇢ webSMLM" streams to it over a
// BroadcastChannel into webSMLM's own live-stream API (window.webSMLM.liveStream.pushChunk). Only a tab of the same
// site can be reached that way; webSMLM's other live input is a WebSocket client, which needs a server a web page
// cannot be. Not committed (gitignored); the Pages build runs this. Default: main, what webSMLM's own site
// (hohlbeinlab.github.io/webSMLM) serves.
//
//   node tools/fetch_websmlm.mjs [--ref <tag|branch>] [--from <local webSMLM.html>]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, 'web', 'websmlm', 'webSMLM.html');
const REPO = 'HohlbeinLab/webSMLM';
const arg = k => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : undefined; };

let html, from;
if (arg('--from')) {
  from = path.resolve(arg('--from'));
  html = fs.readFileSync(from, 'utf8');
} else {
  const ref = arg('--ref') || 'main';
  from = `https://raw.githubusercontent.com/${REPO}/${ref}/webSMLM.html`;
  const r = await fetch(from);
  if (!r.ok) { console.error(`fetch_websmlm: ${from}: HTTP ${r.status}`); process.exit(1); }
  html = await r.text();
}
if (!/window\.webSMLM\.liveStream\s*=/.test(html) || !/id="liveStreamChunkInput"/.test(html)) {
  console.error(`fetch_websmlm: ${from} has no window.webSMLM.liveStream / #liveStreamChunkInput (the API the viewer drives)`);
  process.exit(1);
}
const tag = '<script src="../live/websmlm_bridge.js"></script>\n';
const at = html.lastIndexOf('</body>');
html = at >= 0 ? html.slice(0, at) + tag + html.slice(at) : html + tag;
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, html);
const ver = (html.match(/<span class="pill">v([\d.]+)/) || [])[1] || '?';
console.log(`fetch_websmlm: webSMLM v${ver} (${(html.length / 1e6).toFixed(1)} MB) from ${from} -> ${path.relative(ROOT, OUT)}`);
