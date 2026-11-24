#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The remote launcher (tools/remote.sh) and the server's transports (cli/remote_server.cpp), on Linux:
install from a tarball, check, serve on a Unix socket (a hello over a WebSocket; the server ends when its ssh
session's stdin closes and removes its socket), serve through a "scheduler" (--run-through env: the server on TCP, a
relay on the socket), stdio mode. The job protocol itself: tests/remote/server_check.mjs.
  python3 tests/remote/launcher_check.py --bin-dir build/native/cli        (~10 s)
"""
import argparse, base64, json, os, socket, struct, subprocess, sys, tarfile, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SH = os.path.join(ROOT, 'tools', 'remote.sh')
fails = 0


def ok(cond, what):
    global fails
    print(('ok   ' if cond else 'FAIL ') + what, flush=True)
    if not cond:
        fails += 1


def encode(msg):
    js = json.dumps(dict({'$blobs': []}, **msg)).encode()
    pad = (-(4 + len(js))) % 8
    return struct.pack('<I', len(js)) + js + b'\0' * pad


def decode(buf):
    n = struct.unpack('<I', buf[:4])[0]
    return json.loads(buf[4:4 + n])


def ws_hello(path, token):
    """A WebSocket on the Unix socket: hello, its answer."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(path)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(f'GET /?t={token} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
              f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
    head = b''
    while b'\r\n\r\n' not in head:
        head += s.recv(1)
    if b' 101 ' not in head.split(b'\r\n')[0]:
        s.close()
        return head.split(b'\r\n')[0].decode()
    payload = encode({'type': 'hello', 'id': 0, 'protocol': 1})
    mask = os.urandom(4)
    hdr = bytes([0x82, 0x80 | 126]) + struct.pack('>H', len(payload)) if len(payload) >= 126 else bytes([0x82, 0x80 | len(payload)])
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def recv(n):
        b = b''
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b
    h = recv(2)
    ln = h[1] & 0x7f
    if ln == 126:
        ln = struct.unpack('>H', recv(2))[0]
    elif ln == 127:
        ln = struct.unpack('>Q', recv(8))[0]
    d = decode(recv(ln))
    s.close()
    return d


def running(part):
    """A process whose command line has the announce file of this name part in it (no pgrep in every container)."""
    for d in os.listdir('/proc'):
        if d.isdigit():
            try:
                cmd = open(f'/proc/{d}/cmdline', 'rb').read().replace(b'\0', b' ').decode(errors='replace')
            except OSError:
                continue
            if 'announce-' in cmd and part in cmd and 'insiliscope_server' in cmd:
                return True
    return False


def wait_for(cond, secs):
    t = time.time()
    while time.time() - t < secs:
        if cond():
            return True
        time.sleep(0.05)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin-dir', required=True, help='the folder with insiliscope_server and insiliscope_cli')
    a = ap.parse_args()
    t0 = time.time()
    tmp = tempfile.mkdtemp(prefix='isc-launch-')
    env = dict(os.environ, ISC_REMOTE_HOME=os.path.join(tmp, 'home'))
    tgz = os.path.join(tmp, 'insiliscope-server-linux-x86_64.tar.gz')
    with tarfile.open(tgz, 'w:gz') as t:
        for b in ('insiliscope_server', 'insiliscope_cli'):
            t.add(os.path.join(a.bin_dir, b), f'insiliscope-server-dev/bin/{b}')
        vf = os.path.join(tmp, 'VERSION')
        open(vf, 'w').write('dev\n')
        t.add(vf, 'insiliscope-server-dev/VERSION')
    r = subprocess.run(['sh', SH, 'install', '--version', 'dev', '--from', tgz], env=env, capture_output=True, text=True)
    ok(r.returncode == 0 and os.access(os.path.join(tmp, 'home', 'dev', 'bin', 'insiliscope_server'), os.X_OK), 'install --from a tarball: ' + r.stderr.strip())
    r = subprocess.run(['sh', SH, 'check'], env=env, capture_output=True, text=True)
    ok(r.returncode == 0 and 'CPUs' in r.stdout, 'check:\n' + r.stdout.rstrip())

    # serve on a socket; the ssh session = a pipe on its stdin
    sock = f'/tmp/isc-test{os.getpid()}/s'   # (the viewer's is /tmp/isc-<id>/s)
    p = subprocess.Popen(['sh', SH, 'serve', '--version', 'dev', '--socket', sock, '--token', 'tk', '--threads', '2'],
                         env=env, stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    ok(wait_for(lambda: os.path.exists(sock), 10), 'serve: the socket is there')
    st = os.stat(os.path.dirname(sock))
    ok((st.st_mode & 0o777) == 0o700 and (os.stat(sock).st_mode & 0o777) == 0o600, 'serve: its folder 0700, the socket 0600')
    d = ws_hello(sock, 'tk')
    ok(isinstance(d, dict) and d.get('type') == 'hello', f'serve: a hello over the socket ({d.get("host") if isinstance(d, dict) else d}, {d.get("threads") if isinstance(d, dict) else ""} threads)')
    ok('403' in str(ws_hello(sock, 'wrong')), 'serve: a wrong token is refused')
    p.stdin.close()   # the ssh session ends
    ok(wait_for(lambda: p.poll() is not None, 5), 'serve: ends with its stdin (the ssh session)')
    ok(not os.path.exists(sock) and not os.path.exists(os.path.dirname(sock)), 'serve: its socket and folder removed')

    # through a "scheduler": the server on TCP in the job, a relay on the socket
    p = subprocess.Popen(['sh', SH, 'serve', '--version', 'dev', '--socket', sock, '--token', 'tk2', '--threads', '2', '--run-through', 'env'],
                         env=env, stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    ok(wait_for(lambda: os.path.exists(sock), 15), 'run-through: the relay\'s socket is there')
    d = ws_hello(sock, 'tk2')
    ok(isinstance(d, dict) and d.get('type') == 'hello', 'run-through: a hello through the relay to the job\'s server')
    p.stdin.close()
    ok(wait_for(lambda: p.poll() is not None, 5), 'run-through: ends with its stdin')
    ok(wait_for(lambda: not running(f'-{p.pid}'), 5), 'run-through: the job\'s server ended too')

    # stdio
    p = subprocess.Popen(['sh', SH, 'stdio', '--version', 'dev', '--threads', '2'], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    m = encode({'type': 'hello', 'id': 7, 'protocol': 1})
    p.stdin.write(struct.pack('<I', len(m)) + m)
    p.stdin.flush()
    n = struct.unpack('<I', p.stdout.read(4))[0]
    d = decode(p.stdout.read(n))
    ok(d.get('type') == 'hello' and d.get('id') == 7, 'stdio: a hello')
    p.stdin.close()
    ok(wait_for(lambda: p.poll() is not None, 5), 'stdio: ends with its stdin')

    r = subprocess.run(['sh', SH, 'uninstall'], env=env, capture_output=True, text=True)
    ok(not os.path.exists(os.path.join(tmp, 'home')), 'uninstall')
    print(f'launcher_check: {"all passed" if not fails else str(fails) + " failed"} in {time.time() - t0:.1f} s')
    sys.exit(1 if fails else 0)


main()
