#!/bin/sh
# inSiliScope on a remote Linux server (docs/remote.md): a report to paste back. Run it on the server, e.g. from your PC:
#   ssh isc-server "curl -fsSL https://raw.githubusercontent.com/kjamartens/inSiliScope/remote-server/tests/remote/hpc_check.sh | sh"
# 1. fetches the launcher (tools/remote.sh) at the branch and checks the machine;
# 2. builds the server, the cli and live_bench at the branch (remote.sh build; SKIP_BUILD=1 keeps the last build);
# 3. the GPU: what the system has and what the server finds at its start;
# 4. the cli: the same movies with --gpu off and --gpu auto, timed, the frames compared;
# 5. live frames (live_bench, the viewer's live path): the GPU off and on, timed, the frames hashed.
# Settings: ISC_REF (remote-server), ISC_THREADS (32 per run), SKIP_BUILD (0), KEEP (0: the work folder is removed),
# QUICK (0; 1: small fields, a smoke test of the script itself).
# Takes ~2-4 min after the build (the build: a few minutes).
set -u
REF=${ISC_REF:-remote-server}
RAW=https://raw.githubusercontent.com/kjamartens/inSiliScope/$REF
L=$HOME/.isc-remote.sh
D=${ISC_REMOTE_HOME:-$HOME/.local/share/insiliscope}/dev
T=$(mktemp -d "${TMPDIR:-/tmp}/isc-check-XXXXXX") || exit 1
export ISC_THREADS=${ISC_THREADS:-32}
if [ "${QUICK:-0}" = 1 ]; then S=128 SL=256 F=20 FS=10 LS=128 LL=128; else S=512 SL=1024 F=100 FS=20 LS=256 LL=512; fi
now() { date +%s.%N; }
el() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }'; }
hdr() { printf '\n== %s\n' "$*"; }
T0=$(now)
echo "inSiliScope hpc_check ($REF) on $(hostname) at $(date '+%Y-%m-%d %H:%M'), $ISC_THREADS threads per run"

hdr "1. launcher and machine"
{ curl -fsSL "$RAW/tools/remote.sh" -o "$L" 2>/dev/null || wget -qO "$L" "$RAW/tools/remote.sh"; } ||
  { echo "cannot download $RAW/tools/remote.sh"; exit 1; }
sh "$L" check
echo "  load        $(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null)"

hdr "2. build"
if [ "${SKIP_BUILD:-0}" = 1 ]; then echo "  skipped (SKIP_BUILD=1)"
else t=$(now); sh "$L" build --ref "$REF" 2>&1 | tail -n 4; echo "  build: $(el "$t") s"; fi
echo "  installed: $(cat "$D/VERSION" 2>/dev/null)"
ls -l "$D/bin" "$D/lib" 2>&1 | grep -v '^total' | sed 's/^/  /'
[ -x "$D/bin/insiliscope_cli" ] || { echo "no dev build in $D: stop"; exit 1; }
[ -x "$D/bin/live_bench" ] || echo "  (no live_bench in this build: step 5 is skipped)"

hdr "3. GPU"
if command -v nvidia-smi >/dev/null 2>&1; then
  nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.used,utilization.gpu --format=csv 2>&1 | sed 's/^/  /'
else echo "  no nvidia-smi"; fi
echo "  /dev/dri: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
echo "  Vulkan ICDs: $(ls /usr/share/vulkan/icd.d /etc/vulkan/icd.d 2>/dev/null | grep -v ':$' | tr '\n' ' ')"
command -v vulkaninfo >/dev/null 2>&1 &&
  vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|apiVersion' | head -n 6 | sed 's/^/  /'
[ -f "$D/lib/libwgpu_native.so" ] && { m=$(ldd "$D/lib/libwgpu_native.so" 2>&1 | grep 'not found'); echo "  wgpu-native: $D/lib/libwgpu_native.so${m:+ MISSING: $m}"; }
echo "  the server's start (12 s):"
(sleep 12) | timeout 30 "$D/bin/insiliscope_server" --listen 127.0.0.1:18797 --token hpccheck --threads 2 --gpu auto 2>&1 |
  head -n 4 | sed 's/^/    /'

# Two TIFF movies compared pixel by pixel (16-bit, classic or BigTIFF; numpy if there, else the first 4 M pixels).
cmp_tif() {
  python3 - "$1" "$2" <<'PY'
import struct, sys
def pages(p):
    f = open(p, 'rb').read()
    bo = '<' if f[:2] == b'II' else '>'
    big = struct.unpack(bo + 'H', f[2:4])[0] == 43
    off = struct.unpack(bo + 'Q', f[8:16])[0] if big else struct.unpack(bo + 'I', f[4:8])[0]
    out = []
    while off:
        n = struct.unpack(bo + ('Q' if big else 'H'), f[off:off + (8 if big else 2)])[0]
        p0, es, vs = off + (8 if big else 2), (20 if big else 12), (8 if big else 4)
        tags = {}
        for i in range(n):
            e = p0 + i * es
            tag, typ = struct.unpack(bo + 'HH', f[e:e + 4])
            cnt = struct.unpack(bo + ('Q' if big else 'I'), f[e + 4:e + 4 + vs])[0]
            sz = {3: 2, 4: 4, 16: 8}.get(typ, 0)
            if tag in (273, 279) and sz:
                vo = e + 4 + vs
                if sz * cnt > vs:
                    vo = struct.unpack(bo + ('Q' if big else 'I'), f[vo:vo + vs])[0]
                tags[tag] = struct.unpack(bo + {2: 'H', 4: 'I', 8: 'Q'}[sz] * cnt, f[vo:vo + sz * cnt])
        e = p0 + n * es
        off = struct.unpack(bo + ('Q' if big else 'I'), f[e:e + vs])[0]
        out.append(b''.join(f[o:o + c] for o, c in zip(tags[273], tags[279])))
    return out, bo
(a, bo), (b, _) = pages(sys.argv[1]), pages(sys.argv[2])
if len(a) != len(b) or [len(x) for x in a] != [len(x) for x in b]:
    print('different sizes: %d vs %d pages' % (len(a), len(b))); sys.exit()
A, B = b''.join(a), b''.join(b)
if A == B:
    print('byte for byte the same (%d pages)' % len(a)); sys.exit()
try:
    import numpy as np
    x = np.frombuffer(A, dtype=bo + 'u2').astype(np.int64); y = np.frombuffer(B, dtype=bo + 'u2').astype(np.int64)
    d = np.abs(x - y); n, eq, md, mx, avg = x.size, float((d == 0).mean()), float(d.mean()), int(d.max()), float(x.mean())
except ImportError:
    from array import array
    x, y = array('H', A[:8 << 20]), array('H', B[:8 << 20])
    if (bo == '<') != (sys.byteorder == 'little'):
        x.byteswap(); y.byteswap()
    d = [abs(p - q) for p, q in zip(x, y)]
    n, eq, md, mx, avg = len(d), d.count(0) / len(d), sum(d) / len(d), max(d), sum(x) / len(x)
print('%.2f %% equal, mean |diff| %.3f, max %d, of a mean %.1f ADU (%d px)' % (100 * eq, md, mx, avg, n))
PY
}

cli_case() { # name, the cli's options
  name=$1; shift
  echo "-- $name: $*"
  "$D/bin/insiliscope_cli" --prepare 1 --out "$T/warm.tif" "$@" >/dev/null 2>&1   # (cells packed, caches warm)
  for g in off auto; do
    t=$(now)
    ISC_TIMING=1 "$D/bin/insiliscope_cli" --gpu "$g" --out "$T/$name-$g.tif" "$@" >"$T/$name-$g.log" 2>&1
    rc=$?
    printf '  gpu %-4s %7s s  exit %s\n' "$g" "$(el "$t")" "$rc"
    grep -E 'ISC_TIMING gpu\.create' "$T/$name-$g.log" | head -n 2 | cut -c1-170 | sed 's/^/           /'
    awk '$1 == "ISC_TIMING" && ($2 == "gpu.splat-frames" || $2 == "wf.accel-images" || $2 == "gpu.splat-setup") {
           n[$2]++; s[$2] += $3 }
         END { for (k in n) printf "           %-28s %4d calls, %8.3f s\n", k, n[k], s[k] }' "$T/$name-$g.log"
    [ "$rc" = 0 ] || tail -n 4 "$T/$name-$g.log" | sed 's/^/           /'
  done
  if [ -f "$T/$name-off.tif" ] && [ -f "$T/$name-auto.tif" ]; then
    echo "  frames: $(cmp_tif "$T/$name-off.tif" "$T/$name-auto.tif" 2>&1 | tail -n 1)"
  fi
  rm -f "$T/$name-off.tif" "$T/$name-auto.tif"
}

hdr "4. movies (cli): the CPU (gpu off) vs the GPU (gpu auto); expected: the GPU's within rounding (~99 % equal)"
cli_case dnapaint-$S --size $S --frames $F --x -10 --y 4
cli_case dstorm-$S --size $S --frames $F --x -10 --y 4 --mt-mode 0 --npc-dye None
cli_case widefield-$S --size $S --frames 60 --mt-mode 3 --npc-dye None
cli_case widefield-$SL --size $SL --frames $FS --mt-mode 3 --npc-dye None
cli_case sim2d-widefield-$S --size $S --frames 18 --mt-mode 3 --npc-dye None --sim-type 1

live_case() { # name, expectation, live_bench's spec options
  name=$1; expect=$2; shift 2
  echo "-- live $name: $*  (expected: $expect)"
  for g in off auto; do
    t=$(now)
    ISC_GPU=$g "$D/bin/live_bench" --frames 40 --hash "$@" >"$T/live-$name-$g.txt" 2>&1
    printf '  gpu %-4s %s  (%s s)\n' "$g" "$(grep '^mean' "$T/live-$name-$g.txt" || tail -n 1 "$T/live-$name-$g.txt")" "$(el "$t")"
    grep -E '^ISC_GPU' "$T/live-$name-$g.txt" | head -n 1 | sed 's/^/           /'
  done
  grep '^frame' "$T/live-$name-off.txt" | awk '{ print $NF }' >"$T/h-off"
  grep '^frame' "$T/live-$name-auto.txt" | awk '{ print $NF }' >"$T/h-auto"
  n=$(wc -l <"$T/h-off"); same=$(paste "$T/h-off" "$T/h-auto" | awk '$1 == $2' | wc -l)
  echo "  frames: $same of $n the same, byte for byte"
}

hdr "5. live frames (live_bench, 40 frames): the GPU off vs on"
if [ -x "$D/bin/live_bench" ]; then
  live_case sim2d-widefield-$LS "the same frames and speed: live SIM stays on the CPU" size=$LS mt-mode=3 npc-dye=None sim-type=1
  live_case widefield-$LL "the GPU's within rounding" size=$LL mt-mode=3 npc-dye=None
  live_case dnapaint-$LS "the same: live blinks and noise stay on the CPU" size=$LS x=-10 y=4
else echo "  skipped: no live_bench"; fi

hdr "done in $(el "$T0") s"
if [ "${KEEP:-0}" = 1 ]; then echo "logs kept in $T"; else rm -rf "$T"; fi
