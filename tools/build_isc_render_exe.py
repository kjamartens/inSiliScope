# SPDX-License-Identifier: BSD-3-Clause
"""Builds dist/inSiliScope Render.exe: tools/isc_render.py as one Windows exe that opens its window on a double-click.

It carries insiliscope_cli (built first: cmake --build --preset msvc-cli, ~30 s after a change), the inSiliScope DLL
(adapter/inSiliScope/build/Release/x64: build it with MSBuild first; it reads .cfg files), Micro-Manager's Utilities
adapter (the configurations' Multi Shutter; from pymmcore-plus's Micro-Manager, or --mm-dir), Micro-Manager's core
(pymmcore 12.5.0.75, device interface 75) and the Visual C++ runtime, so it runs on a PC with none of them. The build
runs PyInstaller in a virtual environment of its own (build/isc_render_venv, made on the first run: pyinstaller and
pymmcore and numpy from PyPI), so the system's Python is left alone. The exe holds the GPL-3.0 DLL, so it is GPL-3.0 as a whole
(LICENSE-GPL-3.0.txt); it is a local build, not committed (dist/ is ignored).

  python tools/build_isc_render_exe.py [--no-cli-build] [--mm-dir <Micro-Manager folder>]   (~1 min; ~2 min the first time)
"""

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV = ROOT / "build" / "isc_render_venv"
NAME = "inSiliScope Render"
PYMMCORE = "pymmcore==12.5.0.75.0"   # device interface 75 = the adapter's


def run(cmd, **kw):
    print("  " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--no-cli-build", action="store_true", help="take insiliscope_cli as it is")
    ap.add_argument("--mm-dir", help="a Micro-Manager folder with mmgr_dal_Utilities.dll")
    args = ap.parse_args()
    if os.name != "nt":
        sys.exit("the exe is a Windows build")
    t0 = time.time()

    if not args.no_cli_build:
        print("insiliscope_cli:")
        run(["cmake", "--build", "--preset", "msvc-cli"], cwd=ROOT)
    cli = ROOT / "build" / "msvc" / "cli" / "Release" / "insiliscope_cli.exe"
    dll = ROOT / "adapter" / "inSiliScope" / "build" / "Release" / "x64" / "mmgr_dal_inSiliScope.dll"
    for f in (cli, dll):
        if not f.is_file():
            sys.exit(f"missing {f}")
    mm = args.mm_dir
    if not mm:
        try:
            from pymmcore_plus import find_micromanager
            mm = find_micromanager()
        except Exception:
            mm = r"C:\Program Files\Micro-Manager-2.0"
    utilities = Path(mm or "") / "mmgr_dal_Utilities.dll"
    if not utilities.is_file():
        sys.exit("mmgr_dal_Utilities.dll not found: give --mm-dir (a Micro-Manager folder)")
    system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    runtime = [system / n for n in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")]
    runtime = [f for f in runtime if f.is_file()]

    py = VENV / "Scripts" / "python.exe"
    print("build environment:")
    if not py.is_file():
        run([sys.executable, "-m", "venv", VENV])
    run([py, "-m", "pip", "install", "-q", "--disable-pip-version-check", "pyinstaller", PYMMCORE, "numpy"])   # (pymmcore imports numpy)

    work = ROOT / "build" / "isc_render_pyi"
    sep = os.pathsep
    add = [("--add-binary", f"{cli}{sep}bin"), ("--add-binary", f"{dll}{sep}adapters"),
           ("--add-binary", f"{utilities}{sep}adapters")]
    for f in runtime:   # beside the cli and at the root (the adapter's dependencies resolve there)
        add += [("--add-binary", f"{f}{sep}bin"), ("--add-binary", f"{f}{sep}.")]
    print("PyInstaller:")
    run([py, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed", "--name", NAME,
         "--distpath", ROOT / "dist", "--workpath", work, "--specpath", work, "--hidden-import", "pymmcore", "--hidden-import", "numpy",
         *[x for pair in add for x in pair], ROOT / "tools" / "isc_render.py"], cwd=ROOT)
    exe = ROOT / "dist" / f"{NAME}.exe"
    print(f"\n{exe}  ({exe.stat().st_size / 1e6:.0f} MB, {time.time() - t0:.0f} s)")
    print(f"  carries {cli.name} ({time.ctime(cli.stat().st_mtime)}), {dll.name} ({time.ctime(dll.stat().st_mtime)}), "
          f"{utilities.name}, the VC++ runtime ({len(runtime)} files)")


if __name__ == "__main__":
    main()
