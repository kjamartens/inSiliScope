#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer with a remote server (docs/remote.md; web/live/remote.js, remote_proxy.js; cli/remote_server.cpp): the
// page finds the server by itself, live frames and a Record come from there (the info line names it), the Record's
// frames are the ones this browser makes byte for byte, the switch off brings everything back here (the live sample
// going on), the box's command, and a server that stops (its "ssh session", stdin, closed) sends the engines back here.
//   node tests/web/viewer_remote.mjs --server="build/native/cli/insiliscope_server"
//   node tests/web/viewer_remote.mjs --server="wsl -d Ubuntu -- /home/me/isc-build/cli/insiliscope_server"   (Windows)
//   node tests/web/viewer_remote.mjs --url=ws://127.0.0.1:18799/?t=<32 hex>      (one already running: no stop check)
// ~60 s.
import { spawn } from 'node:child_process';
import { createServer, connect } from 'node:net';
import { randomBytes } from 'node:crypto';
import { runViewer } from './browser.mjs';

const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k + '=')); return a ? a.slice(k.length + 3) : null; };
let port, token, child = null;
if (arg('url')) {
  const m = /^ws:\/\/127\.0\.0\.1:(\d+)\/\?t=([0-9a-f]{32})$/.exec(arg('url'));
  if (!m) { console.error('the url must be ws://127.0.0.1:PORT/?t=<32 hex digits> (the page\'s form of a token)'); process.exit(2); }
  [, port, token] = m;
  port = +port;
} else {
  const cmd = (arg('server') || '').split(' ').filter(Boolean);
  if (!cmd.length) { console.error('give --server="<insiliscope_server command>" or --url=ws://127.0.0.1:PORT/?t=TOKEN'); process.exit(2); }
  port = await new Promise(r => { const s = createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
  token = randomBytes(16).toString('hex');
  // stdin a pipe, as an ssh session's: closing it stops the server
  child = spawn(cmd[0], [...cmd.slice(1), '--listen', `127.0.0.1:${port}`, '--token', token, '--threads', '4', '--idle-exit', '5'],
    { stdio: ['pipe', 'pipe', 'pipe'] });
  // (its words through this process: a child writing to the same file at its own offset overwrote these lines)
  for (const st of [child.stdout, child.stderr]) st.on('data', b => { for (const l of String(b).split(/\r?\n/)) if (l.trim()) console.log('  server: ' + l); });
  const up = () => new Promise(r => { const c = connect(port, '127.0.0.1', () => { c.destroy(); r(true); }); c.on('error', () => r(false)); });
  let ok = false;
  for (let i = 0; i < 200 && !(ok = await up()); i++) await new Promise(r => setTimeout(r, 50));
  if (!ok) { console.error('the server did not start'); child.kill(); process.exit(1); }
}
process.on('exit', () => { if (child) child.kill(); });

// the page's remote settings, put in its storage before its scripts run
const init = ([port, token]) => {
  if (!localStorage.getItem('isc.remote'))
    localStorage.setItem('isc.remote', JSON.stringify({ enabled: true, server: 'isc-test', port, token, sock: 'abcd1234', scope: 'all' }));
};

// (?gpu=0: this browser's Records on the CPU, as the server's without its GPU: byte for byte)
await runViewer('viewer remote', { watchdogSec: 280, viewport: { width: 1200, height: 800 }, query: '?live=0&gpu=0', waitCells: false, initScript: init,
  initArg: [port, token] }, async ({ page, check }) => {
  // a wait that says what the page was doing when it timed out
  const state = () => page.evaluate(() => JSON.stringify({ on: liveState.on, frames: liveState.frames, remote: liveState.remote, err: liveState.err,
    pending: liveState.pending && liveState.pending.type, worker: !!liveState.worker, rec: { on: liveState.rec.on, got: liveState.rec.got, n: liveState.rec.n },
    play: liveState.play.on, msg: document.getElementById('lv_recMsg').textContent, info: document.getElementById('lv_info').textContent,
    remoteOn: self.iscRemote.on(), note: document.getElementById('remoteNote').textContent }));
  const until = async (fn, a, ms) => { try { await page.waitForFunction(fn, a, { timeout: ms }); } catch (e) { throw new Error(e.message.split('\n')[0] + ' -- the page: ' + await state()); } };
  await until(() => self.iscRemote && self.iscRemote.on(), null, 60000);
  const host = await page.evaluate(() => self.iscRemote.host());
  check(!!host, `the page found the server by itself: ${host}; the menu says "${await page.evaluate(() => document.getElementById('remoteNote').textContent)}"`);

  // the box: its command (no $ or backslash: the same in PowerShell, cmd, bash, zsh)
  const cmd = await page.evaluate(() => { self.iscRemote.open(); return document.getElementById('rmCmd').textContent; });
  check(cmd.startsWith(`ssh -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -L ${port}:/tmp/isc-abcd1234/s isc-test "`) &&
    new RegExp(` serve --version \\S+ --socket /tmp/isc-abcd1234/s --token ${token}"$`).test(cmd) && !/[$\\]/.test(cmd),
    `the box's command: ${cmd}`);
  // a page that is not a release (this one) asks for dev and shows the build command first
  const bcmd = await page.evaluate(() => { const e = document.getElementById('rmBuildCmd'); return e ? e.textContent : ''; });
  check(/ serve --version dev /.test(cmd) && bcmd.startsWith('ssh isc-test "') && / build --ref \S+"$/.test(bcmd) && !/[$\\]/.test(bcmd),
    `the build command of a dev page: ${bcmd}`);
  // the server machine's load in the window (a 'status' job every 2 s), and the menu's state light
  await until(() => !document.getElementById('rmLoad').hidden && /%/.test(document.getElementById('rmCpu').textContent), null, 10000);
  const load = await page.evaluate(() => ({ cpu: document.getElementById('rmCpu').textContent, mem: document.getElementById('rmMem').textContent,
    light: document.getElementById('remoteState').dataset.s, folded: !document.getElementById('rmSetup').open }));
  check(/^\d+ % of \d+ CPUs.*inSiliScope \d+ %$/.test(load.cpu) && /[GM]B of .*GB.*inSiliScope .*[GM]B$/.test(load.mem) && load.light === 'on' && load.folded,
    `the window shows the server's load: "${load.cpu}", "${load.mem}", the settings and commands folded; the menu's light: ${load.light}`);
  // a server older than the page (no 'status' job, as before 2026-11-26): it says so where the bars would be
  const old = await page.evaluate(async () => {
    const dec = self.IscWire.decode;
    self.IscWire.decode = b => { const d = dec(b); return d.type === 'status' ? { type: 'status', id: d.id, error: "the remote server does not take 'status' jobs" } : d; };
    self.iscSettings.closeDialog();
    self.iscRemote.open();
    for (let i = 0; i < 50 && document.getElementById('rmOld').hidden; i++) await new Promise(r => setTimeout(r, 100));
    const r = { text: document.getElementById('rmOld').textContent, meters: !document.getElementById('rmMeters').hidden };
    self.IscWire.decode = dec;
    return r;
  });
  check(/older than this viewer/.test(old.text) && !old.meters, `an old server: "${old.text}"`);
  await page.evaluate(() => document.getElementById('rmClose').click());

  await page.evaluate(() => { const s = document.getElementById('mv_size'); s.value = 64; s.dispatchEvent(new Event('input', { bubbles: true })); });
  // live from the server
  await page.evaluate(() => document.getElementById('lv_live').click());
  await until(() => liveState.frames >= 5 && liveState.remote, null, 120000);
  const info = await page.evaluate(() => ({ text: document.getElementById('lv_info').textContent, title: document.getElementById('lv_info').title }));
  check(info.text.endsWith(' · ' + host) && info.title.includes('computed on the remote server ' + host), `live from the server: "${info.text}"`);
  // a live view's frames from the server (job.view, isc_live_view): a 1024 px frame comes in bins down to the view's
  // pixels; zoomed in, the part shown (and a margin), its own pixels
  const setSize = v => page.evaluate(v => { const s = document.getElementById('mv_size'); s.value = v; s.dispatchEvent(new Event('input', { bubbles: true })); }, v);
  const view = () => page.evaluate(() => ({ w: liveState.w, h: liveState.h, n: liveState.frame.length, full: liveState.full, region: liveState.region,
    frames: liveState.frames,
    px: Math.round(document.getElementById('liveView').clientWidth * devicePixelRatio), transform: document.getElementById('lv_cv').style.transform }));
  await setSize(1024);
  await until(() => liveState.remote && liveState.full && liveState.full.w === 1024 && liveState.region, null, 120000);
  const v1 = await view();
  check(v1.region.bin >= 2 && v1.w === Math.floor(1024 / v1.region.bin) && v1.n === v1.w * v1.h && v1.w >= v1.px,
    `live view: a 1024 px frame shown as ${v1.w}×${v1.h} (${v1.region.bin}×${v1.region.bin} bins, the view ${v1.px} px)`);
  await page.evaluate(() => {
    const r = document.getElementById('liveView').getBoundingClientRect();
    document.getElementById('liveView').dispatchEvent(new WheelEvent('wheel', { ctrlKey: true, deltaY: -555, clientX: r.left + r.width / 2, clientY: r.top + r.height / 2, bubbles: true, cancelable: true }));
  });
  await until(() => liveState.region && liveState.region.s < 0.5 && liveState.zoom.z > 3.9, null, 60000);
  const v2 = await view();
  check(Math.abs(v2.region.s - 0.325) < 0.01 && v2.region.bin === 1 && v2.w >= 300 && /scale\(1\.3/.test(v2.transform),
    `zoomed 4×: the part shown and a margin, ${v2.w}×${v2.h} px of the frame from (${(v2.region.x * 1024).toFixed(0)}, ${(v2.region.y * 1024).toFixed(0)}), the canvas ${v2.transform}`);
  await page.evaluate(() => document.getElementById('lv_zoomNav').click());   // the whole image again
  await setSize(64);
  await page.evaluate(() => document.getElementById('lv_live').click());   // off
  await until(() => !liveState.on, null, 10000);

  // a Record (a fresh sample) on the server, then the same here: byte for byte
  const record = async () => {
    const t = Date.now();
    await page.evaluate(() => {
      const n = document.getElementById('lv_recN'); n.value = 6; n.dispatchEvent(new Event('change'));
      document.getElementById('lv_rec').click();
      document.getElementById('lv_recGo').click();
    });
    await until(() => liveState.play.on && !liveState.rec.on, null, 120000);
    const out = await page.evaluate(async () => {
      const frames = [];
      for (let i = 0; i < liveState.play.n; i++) frames.push(Array.from(await liveState.play.store.frame(i)));
      return { frames, remote: !!liveState.rec.remote, disk: !!liveState.play.store.disk };
    });
    await page.evaluate(() => document.getElementById('lv_playClose').click());
    await until(() => !liveState.play.on, null, 10000);
    return { ...out, sec: (Date.now() - t) / 1000 };
  };
  const rr = await record();
  check(rr.remote && rr.frames.length === 6, `a Record of 6 frames on the server (${rr.sec.toFixed(1)} s; frames on disk: ${rr.disk})`);

  // the switch off: back here; the live sample goes on (its light replayed on the new worker)
  const tBefore = await page.evaluate(() => liveState.t);
  const sw = await page.evaluate(() => {
    self.iscRemote.open();
    const on = [document.getElementById('rmGo').checked, document.getElementById('rmBody').hidden, document.getElementById('rmState').textContent];
    document.getElementById('rmGo').click();
    const off = [document.getElementById('rmGo').checked, document.getElementById('rmBody').hidden, document.getElementById('rmState').textContent];
    document.getElementById('rmClose').click();
    return { on, off };
  });
  check(sw.on[0] && !sw.on[1] && /^Connected to /.test(sw.on[2]) && !sw.off[0] && sw.off[1] && sw.off[2] === 'Off',
    `the window's switch: on shows the settings and commands ("${sw.on[2]}"), off only the switch ("${sw.off[2]}")`);
  await until(() => !self.iscRemote.on(), null, 10000);
  const rl = await record();
  const same = rl.frames.length === rr.frames.length && rl.frames.every((f, i) => f.length === rr.frames[i].length && f.every((v, j) => v === rr.frames[i][j]));
  check(!rl.remote && same, `the same Record in this browser: ${rl.frames.length} frames, byte for byte the server's (${rl.sec.toFixed(1)} s)`);
  await page.evaluate(() => document.getElementById('lv_live').click());
  await until(t => liveState.frames >= 3 && !liveState.remote && liveState.t > t, tBefore, 120000);
  check(true, `live here again, the sample's time going on from ${tBefore.toFixed(2)} s`);

  // on again (live moves back to the server), then the server stops: everything back here, live going on
  await page.evaluate(() => { self.iscRemote.open(); document.getElementById('rmGo').click(); document.getElementById('rmClose').click(); });
  await until(() => self.iscRemote.on() && liveState.remote, null, 60000);
  check(true, 'on again: live from the server');
  if (child) {
    child.stdin.end();   // the ssh session ends
    await until(() => !self.iscRemote.on(), null, 20000);
    await until(() => liveState.on && !liveState.remote, null, 60000);
    const f0 = await page.evaluate(() => liveState.frames);
    await until(f => liveState.frames > f + 3, f0, 60000);
    check(true, `the server stopped: live goes on here (the menu: "${await page.evaluate(() => document.getElementById('remoteNote').textContent)}")`);
  }
  await page.evaluate(() => { if (liveState.on) document.getElementById('lv_live').click(); });
});
