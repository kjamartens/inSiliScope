#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# inSiliScope's remote launcher (docs/remote.md): run on a Linux machine you log in to with ssh, it installs the
# inSiliScope server of the version you ask for in your home directory (no admin rights, nothing else needed) and runs
# it. The viewer's Remote server… box and Micro-Manager give the command; you can also run it by hand:
#   sh remote.sh check                       what this machine has (writes nothing)
#   sh remote.sh serve --version v0.2.0 --socket /tmp/isc-ab12cd34/s --token T [--threads N] [--run-through 'srun ...']
#   sh remote.sh stdio --version v0.2.0      one client on stdin/stdout (Micro-Manager)
#   sh remote.sh render --version v0.2.0 < run.isc.json > run.tif
#   sh remote.sh install --version v0.2.0 [--from insiliscope-server-linux-x86_64.tar.gz]
#   sh remote.sh uninstall
# Versions live in ~/.local/share/insiliscope/<version>/ (the last three used are kept; ISC_REMOTE_HOME moves it).
set -u

REPO=kjamartens/inSiliScope
ASSET=insiliscope-server-linux-x86_64.tar.gz
PREFIX=${ISC_REMOTE_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/insiliscope}
MIN_GLIBC=2.28

say() { printf '%s\n' "$*" >&2; }
die() { say "inSiliScope: $*"; exit 1; }

CMD=${1:-help}
[ $# -gt 0 ] && shift
VERSION=latest THREADS= SOCKET= TOKEN= IDLE=30 FROM= RUN_THROUGH= MEMORY= OUT=
while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION=$2; shift 2 ;;
    --threads) THREADS=$2; shift 2 ;;
    --socket) SOCKET=$2; shift 2 ;;
    --token) TOKEN=$2; shift 2 ;;
    --idle-exit) IDLE=$2; shift 2 ;;
    --from) FROM=$2; shift 2 ;;
    --run-through) RUN_THROUGH=$2; shift 2 ;;
    --memory-gb) MEMORY=$2; shift 2 ;;
    --prefix) PREFIX=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    *) die "unknown option $1 (sh remote.sh help)" ;;
  esac
done

# ---- this machine ----
glibc() { getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}'; }
older() { # $1 < $2 (dotted versions)
  [ "$1" = "$2" ] && return 1
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -n1)" = "$1" ]
}
cpus() { nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1; }
platform() {
  [ "$(uname -s)" = Linux ] || die "this machine runs $(uname -s); the inSiliScope server needs Linux"
  case "$(uname -m)" in x86_64|amd64) ;; *) die "this machine is $(uname -m); the inSiliScope server is built for x86-64" ;; esac
  g=$(glibc)
  [ -n "$g" ] || die "no glibc found (musl? Alpine?): the server needs glibc $MIN_GLIBC or newer"
  older "$g" "$MIN_GLIBC" && die "glibc $g found, $MIN_GLIBC needed: this system is too old (a container: see docs/remote.md)"
  return 0
}
fetch() { # url file
  if command -v curl >/dev/null 2>&1; then curl -fsSL -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then wget -qO "$2" "$1"
  else return 1; fi
}

# ---- versions ----
tagof() { case "$1" in latest|dev) echo "$1" ;; v*) echo "$1" ;; *) echo "v$1" ;; esac; }
prune() { # keep the three versions used last (and dev)
  ls -1dt "$PREFIX"/v* 2>/dev/null | tail -n +4 | while read -r d; do rm -rf "$d"; done
}
unpack() { # tarball tag -> $PREFIX/<tag> (its VERSION when tag is latest)
  tmp="$PREFIX/.unpack-$$"
  rm -rf "$tmp"; mkdir -p "$tmp" || die "cannot write $PREFIX"
  tar xzf "$1" -C "$tmp" || { rm -rf "$tmp"; die "cannot unpack $1"; }
  top=$(ls -1 "$tmp" | head -n1)
  [ -x "$tmp/$top/bin/insiliscope_server" ] || { rm -rf "$tmp"; die "$1 holds no bin/insiliscope_server"; }
  t=$2
  [ "$t" = latest ] && t=$(tr -d ' \r\n' < "$tmp/$top/VERSION")
  "$tmp/$top/bin/insiliscope_server" --version >/dev/null 2>&1 || { rm -rf "$tmp"; die "the server does not run here (glibc $(glibc))"; }
  rm -rf "$PREFIX/$t"
  mv "$tmp/$top" "$PREFIX/$t"
  rm -rf "$tmp"
  echo "$t"
}
install() { # -> prints the version's directory
  platform
  tag=$(tagof "$VERSION")
  mkdir -p "$PREFIX" || die "cannot make $PREFIX"
  if [ -n "$FROM" ]; then
    t=$(unpack "$FROM" "$tag") || exit 1
    echo "$PREFIX/$t"; return 0
  fi
  if [ "$tag" != latest ] && [ -x "$PREFIX/$tag/bin/insiliscope_server" ]; then
    touch "$PREFIX/$tag"; echo "$PREFIX/$tag"; return 0
  fi
  [ "$tag" = dev ] && die "no dev build in $PREFIX/dev (install one: sh remote.sh install --version dev --from <tarball>)"
  if [ "$tag" = latest ]; then url="https://github.com/$REPO/releases/latest/download/$ASSET"
  else url="https://github.com/$REPO/releases/download/$tag/$ASSET"; fi
  say "inSiliScope: downloading $tag ... (first time only)"
  tgz="$PREFIX/.download-$$.tar.gz"
  fetch "$url" "$tgz" || { rm -f "$tgz"; die "cannot download $url (no internet here? see 'Servers without internet' in docs/remote.md)"; }
  if fetch "$url.sha256" "$tgz.sha256" 2>/dev/null && command -v sha256sum >/dev/null 2>&1; then
    want=$(awk '{print $1}' "$tgz.sha256"); got=$(sha256sum "$tgz" | awk '{print $1}')
    [ "$want" = "$got" ] || { rm -f "$tgz" "$tgz.sha256"; die "the download's checksum does not match: try again"; }
  fi
  t=$(unpack "$tgz" "$tag") || { rm -f "$tgz" "$tgz.sha256"; exit 1; }
  rm -f "$tgz" "$tgz.sha256"
  prune
  say "inSiliScope: $t installed in $PREFIX/$t"
  echo "$PREFIX/$t"
}
threads() { # the threads per engine: --threads, else half the CPUs
  if [ -n "$THREADS" ]; then echo "$THREADS"; else n=$(cpus); n=$((n / 2)); [ "$n" -lt 1 ] && n=1; echo "$n"; fi
}

case "$CMD" in
  check)
    platform
    printf 'inSiliScope remote check\n'
    printf '  system      %s, glibc %s, %s\n' "$( (. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") || uname -sr)" "$(glibc)" "$(uname -m)"
    printf '  CPUs        %s (load %s)\n' "$(cpus)" "$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)"
    printf '  memory      %s GB (%s GB free)\n' "$(awk '/MemTotal/ {printf "%.0f", $2/1048576}' /proc/meminfo)" "$(awk '/MemAvailable/ {printf "%.0f", $2/1048576}' /proc/meminfo)"
    d=$PREFIX; while [ ! -d "$d" ]; do d=$(dirname "$d"); done
    printf '  disk        %s: %s free\n' "$PREFIX" "$(df -h "$d" 2>/dev/null | awk 'NR==2 {print $4}')"
    if (command -v curl >/dev/null 2>&1 && curl -fsSI --max-time 10 https://github.com >/dev/null 2>&1) ||
       (command -v wget >/dev/null 2>&1 && wget -q --spider --timeout=10 https://github.com 2>/dev/null); then
      printf '  internet    github.com reachable\n'
    else printf '  internet    github.com NOT reachable: install by hand (docs/remote.md)\n'; fi
    s=none; for c in srun sbatch qsub bsub; do command -v $c >/dev/null 2>&1 && { s=$c; break; }; done
    printf '  scheduler   %s\n' "$( [ "$s" = none ] && echo 'none found (runs directly on this machine)' || echo "$s found: use Run through for heavy work")"
    printf '  installed   %s\n' "$(ls -1 "$PREFIX" 2>/dev/null | grep -v '^\.' | tr '\n' ' ')"
    printf 'This machine can run inSiliScope.\n'
    ;;
  install)
    install >/dev/null && say "inSiliScope: ready"
    ;;
  serve)
    [ -n "$SOCKET" ] || die "serve needs --socket PATH"
    [ -n "$TOKEN" ] || die "serve needs --token T"
    dir=$(install) || exit 1
    bin="$dir/bin/insiliscope_server"
    set -- --token "$TOKEN" --threads "$(threads)" --idle-exit "$IDLE"
    [ -n "$MEMORY" ] && set -- "$@" --memory-gb "$MEMORY"
    if [ -z "$RUN_THROUGH" ]; then
      exec nice -n 10 "$bin" --socket "$SOCKET" "$@"
    fi
    # a scheduler's job (srun ...): the server in the job on TCP (its port in a file in the shared home), a relay here
    # from the socket; the job ends with the relay (this ssh session)
    mkdir -p "$PREFIX/run" || die "cannot write $PREFIX/run"
    ann="$PREFIX/run/announce-$(uname -n)-$$"
    rm -f "$ann"
    say "inSiliScope: starting the server through: $RUN_THROUGH"
    # shellcheck disable=SC2086
    $RUN_THROUGH "$bin" --listen 0.0.0.0:0 --announce "$ann" --no-stdin-watch "$@" </dev/null &
    job=$!
    trap 'kill $job 2>/dev/null; rm -f "$ann"' EXIT INT TERM HUP
    waited=0
    while [ ! -s "$ann" ]; do
      kill -0 $job 2>/dev/null || die "the job ended before the server started (see the scheduler's message above)"
      sleep 1; waited=$((waited + 1))
      [ $((waited % 30)) -eq 0 ] && say "inSiliScope: waiting for the job to start ($waited s)"
    done
    set -- $(cat "$ann")
    "$bin" --socket "$SOCKET" --relay "$1:$2" --idle-exit 0
    ;;
  stdio)
    dir=$(install) || exit 1
    exec nice -n 10 "$dir/bin/insiliscope_server" --stdio --threads "$(threads)" ${MEMORY:+--memory-gb "$MEMORY"}
    ;;
  render)
    dir=$(install) || exit 1
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/isc-render-XXXXXX") || die "cannot make a temporary folder"
    trap 'rm -rf "$tmp"' EXIT INT TERM HUP
    cat > "$tmp/run.isc.json"
    [ -s "$tmp/run.isc.json" ] || die "render reads a settings file (.isc.json) on stdin"
    ISC_THREADS=$(threads) nice -n 10 "$dir/bin/insiliscope_cli" --settings "$tmp/run.isc.json" --out "$tmp/${OUT:-movie.tif}" >&2 || die "the render failed"
    cat "$tmp/${OUT:-movie.tif}"
    ;;
  uninstall)
    rm -rf "$PREFIX" && say "inSiliScope: removed $PREFIX"
    ;;
  help|-h|--help)
    sed -n '3,14p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//'
    ;;
  *) die "unknown command $CMD (sh remote.sh help)" ;;
esac
