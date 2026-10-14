// SPDX-License-Identifier: BSD-3-Clause
// Relays the viewer's live frames to webSMLM's Live streaming panel.
//
// webSMLM (HohlbeinLab/webSMLM, webSMLM.html liveStreamWsConnect) connects OUT to a WebSocket server and takes each
// binary message as one chunk of frames (an ImageJ TIFF), a text {"cmd":"stop"} as the end of the session, and answers
// {"cmd":"ack"} / {"cmd":"stopAck"}; the Micro-Manager plugin tools/micromanager_plugin/webSMLM_Streaming there is such
// a server. A browser page cannot listen, so this relay is the server for both sides:
//   ws://localhost:<port>/source   senders (the viewer's "⇢ webSMLM" button, web/live/live.js)
//   ws://localhost:<port>          receivers (webSMLM: Live streaming -> Connect, its default URL ws://localhost:8765)
// Every message from a sender goes to every receiver; a receiver's text (the acks) goes to every sender.
//
//   node tools/websmlm_relay.mjs [--port 8765] [--host localhost]
//
// No dependencies (a minimal RFC 6455 server: unfragmented and fragmented frames, ping/pong, close; no extensions).
// Listens on localhost only by default.
import http from 'http';
import crypto from 'crypto';

const arg = (name, def) => { const i = process.argv.indexOf('--' + name); return i > 0 ? process.argv[i + 1] : def; };
const port = +arg('port', 8765), host = arg('host', 'localhost');
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const senders = new Set(), receivers = new Set();
let chunks = 0, bytes = 0;

// One server frame (never masked): opcode 1 text, 2 binary, 8 close, 9 ping, 10 pong.
function frame(opcode, payload) {
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x80 | opcode, n])
    : n < 65536 ? Buffer.from([0x80 | opcode, 126, n >> 8, n & 255])
      : (() => { const b = Buffer.alloc(10); b[0] = 0x80 | opcode; b[1] = 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })();
  return Buffer.concat([head, payload]);
}
function send(sock, opcode, payload) { if (!sock.destroyed) sock.write(frame(opcode, payload)); }

// Reads client frames (masked) from a socket; calls onMessage(opcode, payload) per whole message.
function reader(sock, onMessage) {
  let buf = Buffer.alloc(0), parts = [], partOp = 0;
  sock.on('data', d => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      const fin = buf[0] & 0x80, op = buf[0] & 0x0f, masked = buf[1] & 0x80;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      const need = off + (masked ? 4 : 0) + len;
      if (buf.length < need) return;
      let payload = buf.subarray(off + (masked ? 4 : 0), need);
      if (masked) {
        const m = buf.subarray(off, off + 4), p = Buffer.alloc(len);
        for (let i = 0; i < len; i++) p[i] = payload[i] ^ m[i & 3];
        payload = p;
      }
      buf = buf.subarray(need);
      if (op >= 8) { onMessage(op, payload); continue; }   // control frames are never fragmented
      if (op !== 0) { partOp = op; parts = []; }
      parts.push(payload);
      if (fin) { onMessage(partOp, Buffer.concat(parts)); parts = []; }
    }
  });
}

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'content-type': 'text/plain' });
  res.end(`inSiliScope -> webSMLM relay: senders ws://${host}:${port}/source, webSMLM ws://${host}:${port}\n`);
});
server.on('upgrade', (req, sock) => {
  const key = req.headers['sec-websocket-key'];
  if (!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket') { sock.destroy(); return; }
  const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
             `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  sock.setNoDelay(true);
  const isSender = (req.url || '/').replace(/\/+$/, '') === '/source';
  const mine = isSender ? senders : receivers, who = isSender ? 'sender' : 'webSMLM';
  mine.add(sock);
  console.log(`${who} connected (${senders.size} sender(s), ${receivers.size} webSMLM)`);
  const drop = () => { if (mine.delete(sock)) console.log(`${who} left (${senders.size} sender(s), ${receivers.size} webSMLM)`); };
  sock.on('close', drop);
  sock.on('error', drop);
  reader(sock, (op, payload) => {
    if (op === 8) { send(sock, 8, Buffer.alloc(0)); sock.end(); drop(); return; }
    if (op === 9) { send(sock, 10, payload); return; }
    if (op !== 1 && op !== 2) return;
    if (isSender) {
      if (op === 2) {
        chunks++; bytes += payload.length;
        if (chunks === 1 || chunks % 100 === 0) console.log(`${chunks} chunk(s) relayed (${(bytes / 1048576).toFixed(1)} MB) to ${receivers.size} webSMLM`);
      } else console.log('sender: ' + payload.toString());
      for (const r of receivers) send(r, op, payload);
    } else if (op === 1) for (const s of senders) send(s, 1, payload);
  });
});
server.listen(port, host, () => console.log(`relay on ws://${host}:${port}: the viewer sends to /source; in webSMLM, Live streaming -> Connect to ws://${host}:${port}`));
