# SPDX-License-Identifier: BSD-3-Clause
"""The ".cfg from the viewer" round trip: a configuration the viewer writes (web/live/mm_cfg.js), loaded in
Micro-Manager, makes the adapter render with the viewer's engine spec.

tests/web/viewer_mm_cfg.mjs --out <dir> drives the viewer through a few scenarios (defaults; dSTORM with dye fields, the
iXon, drift, cell geometry, beads; nanorulers under SIM2D with typed Zernike modes and a flat QE; the slide under a light
sheet with typed BFP maps, a custom dye and other filters) and saves each spec (movieSpec()) and .cfg. Each .cfg is loaded
in pymmcore-plus and Renderer.WriteScopeSpecTo is compared with the spec, key by key: the viewer-only keys
(tools/mm_cfg_map.py VIEWER_ONLY) left out, the viewer's names of the world's params read as Micro-Manager's (ALIASES), a
dye field the spec has at its dye's own value read from its property (Micro-Manager writes only edits), the BFP maps'
text as one line, the light as the Core-Shutter, the Zernike preset as the property.

  ADAPTER_DIR=<dll dir> python tools/test_viewer_cfg.py [--cases <dir>]   (~32 s: the browser part ~26 s)
"""

import json
import logging
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

logging.disable(logging.CRITICAL)   # (pymmcore-plus's log rotation fails while another process holds the log)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from isc_mm import search_paths  # noqa: E402
import mm_cfg_map  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def parse_spec(text):
    """'k=v k="text"' -> {k: float | str} (= settings_core.js parseSpec)."""
    out, i, s = {}, 0, text
    while i < len(s):
        while i < len(s) and s[i] in " \t\r\n,;":
            i += 1
        if i >= len(s):
            break
        m = re.compile(r"[\s,;]").search(s, i)
        end = m.start() if m else len(s)
        eq = s.find("=", i)
        if i < eq < end and eq + 1 < len(s) and s[eq + 1] == '"':
            v, k = "", eq + 2
            while k < len(s) and s[k] != '"':
                if s[k] == "\\" and k + 1 < len(s):
                    k += 1
                    v += "\n" if s[k] == "n" else s[k]
                else:
                    v += s[k]
                k += 1
            out[s[i:eq]] = v
            i = k + 1
            continue
        t = s[i:end]
        j = t.find("=")
        if j > 0:
            out[t[:j]] = float(t[j + 1:])
        i = end
    return out


def bfp_one_line(text):
    """= mm_cfg.js bfpOneLine (the adapter's BfpOneLine with the commas as ';')."""
    out, cur, quoted, comment = [], "", False, False

    def flush():
        nonlocal cur
        t = cur.strip(" \t\r")
        if t:
            out.append(t)
        cur = ""
    for c in text:
        if c == "\n":
            flush()
            comment = quoted = False
            continue
        if comment:
            continue
        if c in ";," and not quoted:
            flush()
            continue
        if c == '"':
            quoted = not quoted
        elif c == "#" and not quoted:
            comment = True
            continue
        cur += c
    flush()
    return "; ".join(out)


def table():
    js = (ROOT / "web" / "live" / "mm_config.js").read_text(encoding="utf-8")
    return json.loads(js[js.index("self.ISC_MM_CFG = ") + len("self.ISC_MM_CFG = "):].rstrip().rstrip(";"))


def engine_defaults():
    """The engine's options and defaults (insiliscope_cli --help; ISC_CLI, else the msvc build's)."""
    cli = os.environ.get("ISC_CLI") or str(ROOT / "build" / "msvc" / "cli" / "Release" / "insiliscope_cli.exe")
    if not Path(cli).exists():
        return {}
    out = subprocess.run([cli, "--help"], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    return {m.group(1): float(m.group(2)) for m in re.finditer(r"^  --([a-z0-9.-]+)\s+(-?[0-9.e+-]+)\s", out, re.M)}


def close(a, b):
    return abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def main():
    from pymmcore_plus import CMMCorePlus

    if not os.environ.get("ADAPTER_DIR"):
        sys.exit("set ADAPTER_DIR to the built adapter's folder")
    cfg = table()
    defaults = engine_defaults()
    prop_of = {o: r for r in cfg["rows"] for o in r["opts"]}
    tmp = tempfile.TemporaryDirectory()
    if "--cases" in sys.argv:
        cases = Path(sys.argv[sys.argv.index("--cases") + 1])
    else:
        cases = Path(tmp.name) / "cases"
        r = subprocess.run(["node", str(ROOT / "tests" / "web" / "viewer_mm_cfg.mjs"), "--out", str(cases)],
                           cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
        print(r.stdout.rstrip())
        if r.returncode:
            sys.exit("the viewer part failed")
    fails = 0
    for spec_file in sorted(cases.glob("*.spec.txt")):
        name = spec_file.name[:-len(".spec.txt")]
        want = parse_spec(spec_file.read_text(encoding="utf-8"))
        core = CMMCorePlus()
        core.setDeviceAdapterSearchPaths(search_paths())
        core.loadSystemConfiguration(str(cases / (name + ".cfg")))
        out = Path(tmp.name) / (name + ".mm.txt")
        core.setProperty("Renderer", "WriteScopeSpecTo", str(out))
        got = parse_spec(" ".join(out.read_text(encoding="utf-8").splitlines()))
        bad = []
        for k, v in sorted(want.items()):
            if k in mm_cfg_map.VIEWER_ONLY or k in ("modality", "light-epi", "light-trans", "psf-zernike-preset"):
                continue
            if k in ("camera-type", "em-gain", "qe-curve") and v == -1:
                continue   # (the camera preset's: Micro-Manager writes it resolved)
            mk = mm_cfg_map.ALIASES.get(k, k)
            if k == "bfp-maps":
                if bfp_one_line(v) != got.get(k, ""):
                    bad.append(f"bfp-maps {bfp_one_line(v)!r} -> {got.get(k)!r}")
                continue
            if mk in got:
                if not close(v, got[mk]):
                    bad.append(f"{k} {v:g} -> {got[mk]:g}")
                continue
            r = prop_of.get(mk)
            if r and r["kind"] == "num":   # a dye field at its dye's value (Micro-Manager writes edits only)
                pv = float(core.getProperty(r["dev"], r["prop"]))
                if not close(v, pv) and round(pv, 4) != round(v, 4):   # (4 decimals: MMDevice's number properties)
                    bad.append(f"{k} {v:g} -> {r['dev']}.{r['prop']} {pv:g}")
                continue
            if mk in defaults and close(v, defaults[mk]):
                continue   # (left out of Micro-Manager's spec: at the engine's default)
            bad.append(f"{k} {v:g}: not in Micro-Manager's spec")
        # the light and the aberrations
        lamp = want.get("modality") == 1
        shutter = "TransmittedLamp" if lamp else "Multi Shutter" if want.get("light-trans") == 1 else "Lasers"
        if core.getShutterDevice() != shutter:
            bad.append(f"Core-Shutter {core.getShutterDevice()} (want {shutter})")
        zp = want.get("psf-zernike-preset")
        if zp is not None and not any(k.startswith("zern.") for k in want):
            name_want = cfg["zernike"][int(zp)]
            if core.getProperty("Objective", "ZernikePreset") != name_want:
                bad.append(f"Objective.ZernikePreset {core.getProperty('Objective', 'ZernikePreset')} (want {name_want})")
        core.unloadAllDevices()
        n = sum(1 for k in want if k not in mm_cfg_map.VIEWER_ONLY)
        print(f"{'ok  ' if not bad else 'FAIL'}  {name}: {n - len(bad)} of {n} options as the viewer's"
              + ("" if not bad else ": " + "; ".join(bad)))
        fails += bool(bad)
    tmp.cleanup()
    print("test_viewer_cfg: " + ("PASS" if not fails else "FAILED"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
