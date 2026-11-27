#!/bin/sh
# inSiliScope on a remote Linux server (docs/remote.md): a report to paste back. Run it on the server, e.g. from your PC:
#   ssh isc-server "curl -fsSL https://raw.githubusercontent.com/kjamartens/inSiliScope/remote-server/tests/remote/hpc_check.sh | sh"
# 1. fetches the launcher (tools/remote.sh) at the branch and checks the machine;
# 2. builds the server, the cli and live_bench at the branch (remote.sh build; SKIP_BUILD=1 keeps the last build);
# 3. the server's start;
# 4. the cli: movies of a few modes and sizes, timed, with the engine's slowest phases;
# 5. live frames (live_bench, the viewer's live path) at growing fields, timed per frame.
# Settings: ISC_REF (remote-server), ISC_THREADS (32 per run), SKIP_BUILD (0), KEEP (0: the work folder is removed),
# QUICK (0; 1: small fields, a smoke test of the script itself).
# Takes ~3-5 min after the build (the build: a minute or a few).
set -u
REF=${ISC_REF:-remote-server}
RAW=https://raw.githubusercontent.com/kjamartens/inSiliScope/$REF
L=$HOME/.isc-remote.sh
D=${ISC_REMOTE_HOME:-$HOME/.local/share/insiliscope}/dev
T=$(mktemp -d "${TMPDIR:-/tmp}/isc-check-XXXXXX") || exit 1
export ISC_THREADS=${ISC_THREADS:-32}
if [ "${QUICK:-0}" = 1 ]; then S=128 SL=256 F=20 FL=40 FW=20 FS=18 LIVE="128 256"
else S=512 SL=1024 F=100 FL=500 FW=200 FS=90 LIVE="512 1024 2048"; fi
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
ls -l "$D/bin" 2>&1 | grep -v '^total' | sed 's/^/  /'
[ -x "$D/bin/insiliscope_cli" ] || { echo "no dev build in $D: stop"; exit 1; }
[ -x "$D/bin/live_bench" ] || echo "  (no live_bench in this build: step 5 is skipped)"

hdr "3. the server's start (5 s)"
(sleep 5) | timeout 20 "$D/bin/insiliscope_server" --listen 127.0.0.1:18797 --token hpccheck --threads 2 2>&1 |
  head -n 4 | sed 's/^/    /'

cli_case() { # name, the cli's options
  name=$1; shift
  echo "-- $name: $*"
  "$D/bin/insiliscope_cli" --prepare 1 --out "$T/warm.tif" "$@" >/dev/null 2>&1   # (cells packed, caches warm)
  t=$(now)
  ISC_TIMING=1 "$D/bin/insiliscope_cli" --out "$T/$name.tif" "$@" >"$T/$name.log" 2>&1
  rc=$?
  printf '  %7s s  exit %s\n' "$(el "$t")" "$rc"
  # the five phases that took longest in all
  awk '$1 == "ISC_TIMING" && $3 + 0 == $3 { n[$2]++; s[$2] += $3 }
       END { for (k in n) printf "%10.3f %5d %s\n", s[k], n[k], k }' "$T/$name.log" | sort -rn | head -n 5 |
    awk '{ printf "           %-28s %4d calls, %8.3f s\n", $3, $2, $1 }'
  [ "$rc" = 0 ] || tail -n 4 "$T/$name.log" | sed 's/^/           /'
  rm -f "$T/$name.tif"
}

hdr "4. movies (cli)"
cli_case dnapaint-$S --size $S --frames $F --x -10 --y 4
cli_case dnapaint-$SL --size $SL --frames $FL --x -10 --y 4
cli_case dstorm-$SL --size $SL --frames $FW --x -10 --y 4 --mt-mode 0 --npc-dye None
cli_case widefield-$SL --size $SL --frames $FW --mt-mode 3 --npc-dye None
cli_case sim2d-widefield-$SL --size $SL --frames $FS --mt-mode 3 --npc-dye None --sim-type 1

live_case() { # name, live_bench's spec options
  name=$1; shift
  t=$(now)
  "$D/bin/live_bench" --frames 30 "$@" >"$T/live-$name.txt" 2>&1
  printf '  %-22s %s  (%s s)\n' "$name" "$(grep '^mean' "$T/live-$name.txt" || tail -n 1 "$T/live-$name.txt")" "$(el "$t")"
}

hdr "5. live frames (live_bench, 30 frames)"
if [ -x "$D/bin/live_bench" ]; then
  for n in $LIVE; do
    live_case dnapaint-$n size=$n x=-10 y=4
    live_case dstorm-$n size=$n x=-10 y=4 mt-mode=0 npc-dye=None
    live_case widefield-$n size=$n mt-mode=3 npc-dye=None
  done
  live_case sim2d-widefield-512 size=512 mt-mode=3 npc-dye=None sim-type=1
else echo "  skipped: no live_bench"; fi

hdr "done in $(el "$T0") s"
if [ "${KEEP:-0}" = 1 ]; then echo "logs kept in $T"; else rm -rf "$T"; fi
