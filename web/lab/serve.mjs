// Dev server for the lab (zero dependencies).
//   node web/lab/serve.mjs [--port 8123] [--base origin/main]
// Serves the repo root; /baseline/<path> serves <path> as of the git ref --base (A/B against main);
// /__lab/events is a server-sent-event stream that fires on every save under web/prototype or web/lab.
import http from 'http';
import fs from 'fs';
import path from 'path';
import { execFileSync } from 'child_process';
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
for (const dir of ['web/prototype', 'web/lab']) {
  fs.watch(path.join(ROOT, dir), { recursive: true }, (_, f) => {
    clearTimeout(timer);
    timer = setTimeout(() => {
      const msg = `data: ${JSON.stringify({ dir, file: String(f || '') })}\n\n`;
      for (const c of clients) c.write(msg);
    }, 80);
  });
}

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  let p = decodeURIComponent(url.pathname);
  if (p === '/') { res.writeHead(302, { Location: '/web/lab/' }); return res.end(); }
  if (p === '/__lab/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive' });
    res.write(': hi\n\n');
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }
  if (p === '/__lab/info') {
    let head = '', dirty = '';
    try { head = git('rev-parse', '--abbrev-ref', 'HEAD').toString().trim(); } catch {}
    try { dirty = git('status', '--porcelain', '--', 'web/prototype').toString(); } catch {}
    return send(res, 200, JSON.stringify({ base: BASE, baseSha, head, prototypeDirty: dirty.trim().split('\n').filter(Boolean) }), 'application/json');
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
}).listen(PORT, () => {
  console.log(`lab: http://localhost:${PORT}/web/lab/   (baseline ${BASE ? `${BASE} @ ${baseSha}` : 'none'})`);
});
