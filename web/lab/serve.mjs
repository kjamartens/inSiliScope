// Dev server for the lab (zero dependencies).
//   node web/lab/serve.mjs [--port 8123] [--base origin/main] [--open]
// --open opens /web/lab.html in the default browser (`cmake --build --preset lab` passes it). A lab already on the
// port is stopped and replaced (its open pages reload onto the new one; --open then opens no extra tab).
// Serves the repo root. /web/lab.html is web/index.html (the viewer) on the JS reference: its WASM module tag
// becomes self.ISC_ENGINE_URL = 'lab/engine.js', plus lab/lab_html.js (badge, reload on save). /web/lab/ is the
// A/B page; /baseline/<path> serves <path> as of the git ref --base (A/B against main); /__lab/info is the checkout
// (branch, commit, uncommitted files: lab.html's About line) and the baseline ref; /__lab/events is a
// server-sent-event stream that fires on every save under web/prototype, web/lab, or of web/index.html / wf_gpu.js.
import http from 'http';
import fs from 'fs';
import path from 'path';
import { execFileSync, spawn } from 'child_process';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const PORT = +arg('--port', 8123);
let BASE = arg('--base', 'origin/main');
const git = (...a) => execFileSync('git', a, { cwd: ROOT, encoding: 'buffer', stdio: ['ignore', 'pipe', 'ignore'] });
try { git('rev-parse', '--verify', BASE); } catch { BASE = 'main'; }
let baseSha = '';
try { baseSha = git('rev-parse', '--short', BASE).toString().trim(); } catch { BASE = null; }

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.wgsl': 'text/plain', '.png': 'image/png', '.svg': 'image/svg+xml', '.md': 'text/plain' };
const send = (res, code, body, type = 'text/plain') => {
  res.writeHead(code, { 'Content-Type': type + '; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(body);
};

const clients = new Set();
let timer = null;
const notify = (dir, f) => {
  clearTimeout(timer);
  timer = setTimeout(() => {
    const msg = `data: ${JSON.stringify({ dir, file: String(f || '') })}\n\n`;
    for (const c of clients) c.write(msg);
  }, 80);
};
for (const dir of ['web/prototype', 'web/lab']) fs.watch(path.join(ROOT, dir), { recursive: true }, (_, f) => notify(dir, f));
fs.watch(path.join(ROOT, 'web'), (_, f) => { if (f === 'index.html' || f === 'wf_gpu.js') notify('web', f); });

const MODULE_TAG = '<script src="insiliscope_module.js"></script>';
function labHtml() {
  const html = fs.readFileSync(path.join(ROOT, 'web/index.html'), 'utf8');
  if (!html.includes(MODULE_TAG)) throw new Error('web/index.html: ' + MODULE_TAG + ' not found');
  return html.replace(MODULE_TAG, "<script>self.ISC_ENGINE_URL = 'lab/engine.js';</script>")
    .replace('</body>', '<script src="lab/lab_html.js"></script>\n</body>');
}

// This server's identity: sent as a `hello` event to every page, which reloads when it changes (a restart).
const START_ID = `${process.pid}-${Date.now()}`;
let pagesSeen = 0;

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  let p = decodeURIComponent(url.pathname);
  if (p === '/') { res.writeHead(302, { Location: '/web/lab/' }); return res.end(); }
  if (p === '/__lab/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive' });
    res.write(`retry: 500\nevent: hello\ndata: ${START_ID}\n\n`);  // reconnect fast after a restart
    clients.add(res);
    pagesSeen++;
    req.on('close', () => clients.delete(res));
    return;
  }
  if (p === '/__lab/quit') {   // a newer lab on this port takes over (see takeOver)
    send(res, 200, 'bye');
    console.log('lab: stopped by a new lab instance');
    setTimeout(() => process.exit(0), 50);
    return;
  }
  if (p === '/__lab/info') {
    let head = '', dirty = '', sha = '', subject = '', changed = '';
    try { head = git('rev-parse', '--abbrev-ref', 'HEAD').toString().trim(); } catch {}
    try { dirty = git('status', '--porcelain', '--', 'web/prototype').toString(); } catch {}
    try { [sha, subject] = git('log', '-1', '--format=%h%n%s').toString().trim().split('\n'); } catch {}
    try { changed = git('status', '--porcelain', '--untracked-files=no').toString(); } catch {}
    return send(res, 200, JSON.stringify({ base: BASE, baseSha, head, sha, subject,
      changedFiles: changed.trim().split('\n').filter(Boolean).length,
      prototypeDirty: dirty.trim().split('\n').filter(Boolean) }), 'application/json');
  }
  if (p === '/web/lab.html') {
    try { return send(res, 200, labHtml(), 'text/html'); } catch (e) { return send(res, 500, String(e.message)); }
  }
  const ext = path.extname(p).toLowerCase() || '.html';
  if (p.startsWith('/baseline/')) {
    if (!BASE) return send(res, 404, 'no baseline ref');
    try { return send(res, 200, git('show', `${BASE}:${p.slice('/baseline/'.length)}`), TYPES[ext] || 'application/octet-stream'); }
    catch { return send(res, 404, 'not in ' + BASE); }
  }
  if (p.endsWith('/')) p += 'index.html';
  const f = path.join(ROOT, p);
  if (!f.startsWith(ROOT)) return send(res, 403, 'no');
  fs.readFile(f, (err, buf) => err ? send(res, 404, 'not found') : send(res, 200, buf, TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream'));
});

// Port taken: if it is a lab (answers /__lab/info), stop it -- /__lab/quit, or, for a lab from before that
// endpoint, the process listening on the port -- and take its place. Anything else on the port: give up.
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function takeOver() {
  const lab = `http://localhost:${PORT}/__lab/`;
  try {
    const info = await (await fetch(lab + 'info', { signal: AbortSignal.timeout(2000) })).json();
    if (!('head' in info)) return false;
  } catch { return false; }
  let quit = false;
  try { quit = (await fetch(lab + 'quit', { signal: AbortSignal.timeout(2000) })).ok; } catch {}
  if (!quit) {
    let pids = [];
    try {
      if (process.platform === 'win32') {
        const re = new RegExp(`^\\s*TCP\\s+\\S+:${PORT}\\s+\\S+\\s+LISTENING\\s+(\\d+)`, 'gm');
        pids = [...execFileSync('netstat', ['-ano', '-p', 'tcp'], { encoding: 'utf8' }).matchAll(re)].map(m => +m[1]);
      } else {
        pids = execFileSync('lsof', ['-ti', `tcp:${PORT}`, '-sTCP:LISTEN'], { encoding: 'utf8' }).split(/\s+/).filter(Boolean).map(Number);
      }
    } catch {}
    for (const pid of new Set(pids)) if (pid && pid !== process.pid) try { process.kill(pid); } catch {}
  }
  console.log(`lab: stopped the lab that was running on port ${PORT}`);
  return true;
}
function listen() {
  return new Promise((resolve, reject) => {
    const fail = e => { server.off('listening', ok); reject(e); };
    const ok = () => { server.off('error', fail); resolve(); };
    server.once('error', fail).once('listening', ok).listen(PORT);
  });
}

let tookOver = false;
try {
  await listen();
} catch (e) {
  if (e.code !== 'EADDRINUSE') throw e;
  if (!(await takeOver())) {
    console.error(`lab: port ${PORT} is in use by something that is not a lab. Pass --port <n>.`);
    process.exit(1);
  }
  tookOver = true;
  for (let i = 0; ; i++) {   // the old one needs a moment to let go of the port
    try { await listen(); break; } catch (e2) {
      if (e2.code !== 'EADDRINUSE' || i >= 40) { console.error(`lab: port ${PORT} still in use. Pass --port <n>.`); process.exit(1); }
      await sleep(150);
    }
  }
}
const page = `http://localhost:${PORT}/web/lab.html`;
console.log(`lab: ${page}   (the viewer on the JS reference; reloads on save)`);
console.log(`     http://localhost:${PORT}/web/lab/   (A/B against ${BASE ? `${BASE} @ ${baseSha}` : 'no baseline'})   Ctrl+C stops it`);
if (process.argv.includes('--open')) {
  // After a take-over, pages of the old lab reconnect and reload by themselves (the `hello` event); open a new
  // tab only if none came back.
  if (tookOver) await sleep(4000);
  if (!tookOver || pagesSeen === 0) {
    const [cmd, args] = process.platform === 'win32' ? ['cmd', ['/c', 'start', '', page]]
      : process.platform === 'darwin' ? ['open', [page]] : ['xdg-open', [page]];
    spawn(cmd, args, { stdio: 'ignore', detached: true }).on('error', () => {}).unref();
  } else console.log(`lab: ${pagesSeen} open page(s) reconnected; not opening another`);
}
