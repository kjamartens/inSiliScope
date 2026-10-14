// tools/websmlm_relay.mjs: a sender's binary chunk and its {"cmd":"stop"} reach a receiver (as webSMLM gets them), a
// receiver's ack reaches the sender, large chunks (> 64 KB: the 64-bit length) arrive whole.
//   node tests/web/websmlm_relay_check.mjs        (Node >= 22: the global WebSocket client)
import { spawn } from 'child_process';
import path from 'path';
import { fileURLToPath } from 'url';

const t0 = performance.now();
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const port = 20000 + Math.floor(Math.random() * 20000);
const relay = spawn(process.execPath, [path.join(ROOT, 'tools/websmlm_relay.mjs'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((res, rej) => { relay.stdout.once('data', res); relay.once('exit', c => rej(new Error('relay exited ' + c))); });
let fails = 0;
const check = (ok, msg) => { console.log((ok ? 'ok   ' : 'FAIL ') + msg); if (!ok) fails++; };
const open = url => new Promise((res, rej) => { const ws = new WebSocket(url); ws.binaryType = 'arraybuffer'; ws.onopen = () => res(ws); ws.onerror = rej; });
try {
  const rx = await open(`ws://localhost:${port}`), tx = await open(`ws://localhost:${port}/source`);
  const got = [];
  rx.onmessage = e => { got.push(e.data); if (typeof e.data !== 'string') rx.send(JSON.stringify({ cmd: 'ack', framesReceived: got.length, ok: true })); };
  const acks = [];
  tx.onmessage = e => acks.push(JSON.parse(e.data));
  const small = new Uint8Array(1000).map((_, i) => i & 255), big = new Uint8Array(300000).map((_, i) => (i * 7) & 255);
  tx.send(small); tx.send(big); tx.send(JSON.stringify({ cmd: 'stop' }));
  const until = performance.now() + 5000;
  while ((got.length < 3 || acks.length < 2) && performance.now() < until) await new Promise(r => setTimeout(r, 20));
  const same = (a, b) => a instanceof ArrayBuffer && a.byteLength === b.length && new Uint8Array(a).every((v, i) => v === b[i]);
  check(got.length === 3 && same(got[0], small) && same(got[1], big), 'two binary chunks (1 KB, 300 KB) arrive whole and in order');
  check(got[2] === '{"cmd":"stop"}', 'the stop text arrives');
  check(acks.length === 2 && acks[1].framesReceived === 2, 'the receiver\'s acks reach the sender');
  rx.close(); tx.close();
} catch (e) { check(false, 'relay: ' + (e.message || e)); }
relay.kill();
console.log(`${fails ? 'FAIL' : 'ok'}: websmlm relay (${((performance.now() - t0) / 1000).toFixed(1)} s)`);
process.exit(fails ? 1 : 0);
