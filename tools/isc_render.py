# SPDX-License-Identifier: BSD-3-Clause
"""inSiliScope render: movies from a saved setup, straight to TIFF on this PC.

Load a viewer settings file (<name>.isc.json, the viewer's Save settings) or a Micro-Manager configuration (.cfg: the
shipped ones, or the viewer's Micro-Manager .cfg export), enter stage positions (x y z, um), the exposure and the number
of frames, and each position becomes one 16-bit TIFF stack (ImageJ / Fiji / napari open it; the spec is in its
description). The frames are insiliscope_cli's, = the viewer's "Make movie here" and Micro-Manager's precomputed frames.

  python tools/isc_render.py                          a window
  python tools/isc_render.py <file> --pos "x y z" [--pos ...] [--exposure-ms 50] [--frames 100] [--start-sec s]
                             [--out dir] [--cli path] [--adapter-dir dir]

A .isc.json goes to the cli as is (its measurement's light history replayed, as the viewer's live sample); a .cfg is
loaded in Micro-Manager's core (pymmcore-plus, the inSiliScope DLL), its Core-Shutter opened, and the spec the adapter
writes (Renderer.WriteScopeSpecTo) goes to the cli. Positions, exposure, frames and the start time replace the file's;
a blank start time keeps the file's (a viewer file's live sample: 60 s; Micro-Manager's spec: 0 s, a fresh sample).

Finds the cli at --cli, $ISC_CLI, next to this script, or the build trees (build/msvc/cli/Release, build/native/cli);
the DLL (only for a .cfg) at --adapter-dir, $ADAPTER_DIR, the build tree or C:/Program Files/Micro-Manager-2.0.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


# ---- finding the programs ----

def find_cli(given=None):
    exe = "insiliscope_cli.exe" if os.name == "nt" else "insiliscope_cli"
    for c in [given, os.environ.get("ISC_CLI"), HERE / exe, ROOT / "build" / "msvc" / "cli" / "Release" / exe,
              ROOT / "build" / "native" / "cli" / exe]:
        if c and Path(c).is_file():
            return str(c)
    raise FileNotFoundError("insiliscope_cli not found: build it (cmake --build --preset msvc-cli) or give --cli")


def adapter_dirs(given=None):
    """Folders that may hold mmgr_dal_inSiliScope (first found wins), then Micro-Manager's own adapters."""
    out = []
    for c in [given, os.environ.get("ADAPTER_DIR"), ROOT / "adapter" / "inSiliScope" / "build" / "Release" / "x64",
              Path(r"C:\Program Files\Micro-Manager-2.0")]:
        if c and any(Path(c).glob("mmgr_dal_inSiliScope*")):
            out.append(str(c))
            break
    if not out:
        raise FileNotFoundError("mmgr_dal_inSiliScope not found: give --adapter-dir (the folder with the DLL)")
    try:   # the Utilities adapter (the Multi Shutter of the configurations): pymmcore-plus's Micro-Manager
        from pymmcore_plus import find_micromanager
        mm = find_micromanager()
        if mm and mm not in out:
            out.append(mm)
    except Exception:
        pass
    return out


# ---- a setup: the file and the values it holds ----

class Setup:
    """A loaded file: kind 'json' (the cli reads it) or 'cfg' (the spec Micro-Manager writes, kept in a file)."""

    def __init__(self, path, adapter_dir=None, log=print):
        self.path = str(Path(path).resolve())
        self.values = {}
        self.spec_file = None
        if self.path.lower().endswith(".cfg"):
            self.kind = "cfg"
            self._from_cfg(adapter_dir, log)
        else:
            self.kind = "json"
            d = json.loads(Path(self.path).read_text(encoding="utf-8"))
            if d.get("format") != "insiliscope-settings":
                raise ValueError(f"{Path(path).name}: not an inSiliScope settings file (.isc.json) or a .cfg")
            for p in ("instrument", "settings", "sample", "measurement"):
                self.values.update((d.get("parts", {}).get(p) or {}).get("values", {}))

    def _from_cfg(self, adapter_dir, log):
        import logging
        logging.disable(logging.CRITICAL)   # (pymmcore-plus's log rotation complains while another process holds it)
        from pymmcore_plus import CMMCorePlus
        t = time.time()
        core = CMMCorePlus()
        core.setDeviceAdapterSearchPaths(adapter_dirs(adapter_dir))
        core.loadSystemConfiguration(self.path)
        light = core.getShutterDevice()
        if light:   # the light the configuration takes images with (its shutters are closed after Startup)
            core.setShutterOpen(True)
        fd, spec = tempfile.mkstemp(prefix="isc_render_", suffix=".spec.txt")
        os.close(fd)
        core.setProperty("Renderer", "WriteScopeSpecTo", spec)
        core.unloadAllDevices()
        self.spec_file = spec
        for line in Path(spec).read_text(encoding="utf-8").splitlines():
            k, eq, v = line.partition("=")
            if eq:
                try:
                    self.values[k.strip()] = float(v)
                except ValueError:
                    self.values[k.strip()] = v.strip()
        log(f"Loaded {Path(self.path).name} in Micro-Manager ({time.time() - t:.1f} s; light: {light or 'none'})")

    def args(self):
        return ["--settings", self.path] if self.kind == "json" else ["--spec", self.spec_file]

    def get(self, key, default):
        v = self.values.get(key, default)
        return v if isinstance(v, (int, float)) else default

    def close(self):
        if self.spec_file:
            try:
                os.unlink(self.spec_file)
            except OSError:
                pass


def parse_positions(text):
    """'x y z' per line (spaces, commas, tabs or semicolons; '#' comments) -> [(x, y, z)] in um."""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = [p for p in re.split(r"[\s,;]+", line) if p]
        if len(parts) != 3:
            raise ValueError(f"position line {n}: want x y z (um), got {line!r}")
        out.append(tuple(float(p) for p in parts))
    if not out:
        raise ValueError("no positions")
    return out


def num(v):
    return f"{v:g}"


def out_name(folder, stem, k, pos):
    x, y, z = pos
    p = Path(folder) / f"{stem}_{k:02d}_x{num(x)}_y{num(y)}_z{num(z)}.tif"
    i = 2
    while p.exists():
        p = p.with_name(f"{stem}_{k:02d}_x{num(x)}_y{num(y)}_z{num(z)}_{i}.tif")
        i += 1
    return p


# ---- rendering: one served cli for every position (it keeps the world and the PSF between them) ----

def render(setup, positions, exposure_ms, frames, start_sec, folder, cli, log=print, stop=None):
    """Renders each position into its TIFF; returns the files. log(text) per line; stop() True cancels between movies."""
    Path(folder).mkdir(parents=True, exist_ok=True)
    stem = re.sub(r"\.(isc\.json|cfg|json)$", "", Path(setup.path).name, flags=re.I)
    proc = subprocess.Popen([cli, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", bufsize=1)
    files = []
    try:
        for k, pos in enumerate(positions, 1):
            if stop and stop():
                log("Stopped.")
                break
            out = out_name(folder, stem, k, pos)
            args = setup.args() + ["--x", num(pos[0]), "--y", num(pos[1]), "--z", num(pos[2]),
                                   "--exposure-ms", num(exposure_ms), "--frames", str(int(frames)), "--out", str(out)]
            if start_sec is not None:
                args += ["--start-sec", num(start_sec)]
            log(f"[{k}/{len(positions)}] x {num(pos[0])}  y {num(pos[1])}  z {num(pos[2])} um ...")
            t = time.time()
            proc.stdin.write("\t".join(args) + "\n")
            proc.stdin.flush()
            code = None
            while True:
                line = proc.stdout.readline()
                if not line:
                    raise RuntimeError("insiliscope_cli stopped")
                m = re.match(r"@@isc-done (-?\d+)", line)
                if m:
                    code = int(m.group(1))
                    break
                line = line.rstrip()
                if line:
                    log("    " + line.replace(str(out), out.name))
            if code:
                raise RuntimeError(f"insiliscope_cli failed (exit {code}) at position {k}")
            files.append(out)
            log(f"    -> {out}  ({time.time() - t:.1f} s)")
    finally:
        try:
            proc.stdin.close()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
    return files


# ---- the window ----

def gui(args):
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    if os.name == "nt":   # crisp text on a scaled display
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    root = tk.Tk()
    root.title("inSiliScope render")
    scale = root.winfo_fpixels("1i") / 96
    root.geometry(f"{int(860 * scale)}x{int(620 * scale)}")
    root.minsize(int(640 * scale), int(480 * scale))
    state = {"setup": None, "busy": False, "stop": False}
    f = ttk.Frame(root, padding=10)
    f.grid(sticky="nsew")
    root.columnconfigure(0, weight=1)
    root.rowconfigure(0, weight=1)
    f.columnconfigure(1, weight=1)

    file_var = tk.StringVar(value=args.file or "")
    out_var = tk.StringVar(value=args.out or str(Path.home() / "inSiliScope renders"))
    exp_var = tk.StringVar(value=num(args.exposure_ms) if args.exposure_ms else "50")
    frames_var = tk.StringVar(value=str(args.frames) if args.frames else "100")
    start_var = tk.StringVar(value=num(args.start_sec) if args.start_sec is not None else "")

    ttk.Label(f, text="Setup (.isc.json or .cfg)").grid(row=0, column=0, sticky="w")
    ttk.Entry(f, textvariable=file_var).grid(row=0, column=1, sticky="ew", padx=4)
    ttk.Button(f, text="Open…", command=lambda: pick()).grid(row=0, column=2)
    info = ttk.Label(f, text="", foreground="gray", wraplength=int(640 * scale))
    info.grid(row=1, column=1, columnspan=2, sticky="w", padx=4)

    ttk.Label(f, text="Positions: x y z (um),\none per line").grid(row=2, column=0, sticky="nw", pady=(6, 0))
    pos_box = tk.Text(f, height=6, width=30)
    pos_box.grid(row=2, column=1, columnspan=2, sticky="nsew", padx=4, pady=(6, 0))
    pos_box.insert("1.0", "\n".join(args.pos) if args.pos else "0 0 0.5")

    g = ttk.Frame(f)
    g.grid(row=3, column=1, columnspan=2, sticky="w", padx=4, pady=6)
    for i, (label, var, w) in enumerate([("Exposure (ms)", exp_var, 8), ("Frames", frames_var, 8),
                                         ("Start time (s; blank = the file's)", start_var, 8)]):
        ttk.Label(g, text=label).grid(row=0, column=2 * i, sticky="w", padx=(0 if i == 0 else 12, 4))
        ttk.Entry(g, textvariable=var, width=w).grid(row=0, column=2 * i + 1)

    ttk.Label(f, text="Output folder").grid(row=4, column=0, sticky="w")
    ttk.Entry(f, textvariable=out_var).grid(row=4, column=1, sticky="ew", padx=4)
    ttk.Button(f, text="Choose…", command=lambda: out_var.set(filedialog.askdirectory(initialdir=out_var.get()) or out_var.get())).grid(row=4, column=2)

    b = ttk.Frame(f)
    b.grid(row=5, column=0, columnspan=3, sticky="ew", pady=8)
    go = ttk.Button(b, text="Render", command=lambda: start())
    go.pack(side="left")
    stop_btn = ttk.Button(b, text="Stop after this position", state="disabled",
                          command=lambda: state.update(stop=True))
    stop_btn.pack(side="left", padx=6)
    open_btn = ttk.Button(b, text="Open output folder", command=lambda: open_folder(out_var.get()))
    open_btn.pack(side="right")
    bar = ttk.Progressbar(f, mode="determinate")
    bar.grid(row=6, column=0, columnspan=3, sticky="ew")

    log_box = tk.Text(f, height=12, state="disabled", wrap="none")
    log_box.grid(row=7, column=0, columnspan=3, sticky="nsew", pady=(6, 0))
    f.rowconfigure(7, weight=1)

    def log(text):
        def put():
            log_box.configure(state="normal")
            log_box.insert("end", text + "\n")
            log_box.see("end")
            log_box.configure(state="disabled")
        root.after(0, put)

    def open_folder(p):
        if Path(p).is_dir():
            if os.name == "nt":
                os.startfile(p)
            else:
                subprocess.Popen(["xdg-open" if sys.platform != "darwin" else "open", p])

    def load(path):
        if not path:
            return
        if state["setup"]:
            state["setup"].close()
            state["setup"] = None
        info.configure(text="Loading…")

        def work():
            try:
                s = Setup(path, args.adapter_dir, log)
            except Exception as e:
                root.after(0, lambda: (info.configure(text=""), messagebox.showerror("inSiliScope render", str(e))))
                return

            def done():
                state["setup"] = s
                x, y, z = s.get("x", 0), s.get("y", 0), s.get("z", 0.5)
                pos_box.delete("1.0", "end")
                pos_box.insert("1.0", f"{num(x)} {num(y)} {num(z)}")
                exp_var.set(num(s.get("exposure-ms", 50)))
                size = int(s.get("size", 128))
                h = int(s.get("height", 0)) or size
                info.configure(text=f"{'viewer settings' if s.kind == 'json' else 'Micro-Manager configuration'}: "
                                    f"{size} x {h} px, the file's stage position below, start time "
                                    f"{num(s.get('start-sec', 60 if s.kind == 'json' else 0))} s")
                log(f"Loaded {Path(path).name}")
            root.after(0, done)
        threading.Thread(target=work, daemon=True).start()

    def pick():
        p = filedialog.askopenfilename(filetypes=[("inSiliScope setups", "*.isc.json *.cfg"), ("All files", "*.*")])
        if p:
            file_var.set(p)
            load(p)

    def start():
        if state["busy"]:
            return
        try:
            if not state["setup"] or state["setup"].path != str(Path(file_var.get()).resolve()):
                raise ValueError("open a setup first")
            positions = parse_positions(pos_box.get("1.0", "end"))
            exposure = float(exp_var.get())
            frames = int(frames_var.get())
            start_sec = float(start_var.get()) if start_var.get().strip() else None
            if exposure <= 0 or frames < 1:
                raise ValueError("the exposure and the number of frames must be positive")
            cli = find_cli(args.cli)
        except Exception as e:
            messagebox.showerror("inSiliScope render", str(e))
            return
        state.update(busy=True, stop=False)
        go.configure(state="disabled")
        stop_btn.configure(state="normal")
        bar.configure(maximum=len(positions), value=0)

        def step(text):
            log(text)
            if text.startswith("    -> "):
                root.after(0, lambda: bar.configure(value=bar.cget("value") + 1))   # (step() wraps at the end)

        def work():
            t = time.time()
            try:
                files = render(state["setup"], positions, exposure, frames, start_sec, out_var.get(), cli, step,
                               lambda: state["stop"])
                log(f"Done: {len(files)} file(s) in {time.time() - t:.1f} s.")
            except Exception as e:
                log(f"Error: {e}")
            finally:
                def idle():
                    state["busy"] = False
                    go.configure(state="normal")
                    stop_btn.configure(state="disabled")
                root.after(0, idle)
        threading.Thread(target=work, daemon=True).start()

    if args.file:
        load(args.file)
    root.protocol("WM_DELETE_WINDOW", lambda: (state["setup"] and state["setup"].close(), root.destroy()))
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file", nargs="?", help="a viewer settings file (.isc.json) or a Micro-Manager configuration (.cfg)")
    ap.add_argument("--pos", action="append", help='a stage position "x y z" in um (repeat for more)')
    ap.add_argument("--exposure-ms", type=float, help="frame exposure, ms (default: the file's)")
    ap.add_argument("--frames", type=int, help="frames per position (default 100)")
    ap.add_argument("--start-sec", type=float, help="simulated time of the first frame, s (default: the file's)")
    ap.add_argument("--out", help="output folder (default: ~/inSiliScope renders)")
    ap.add_argument("--cli", help="insiliscope_cli to use")
    ap.add_argument("--adapter-dir", help="the folder with mmgr_dal_inSiliScope (for a .cfg)")
    ap.add_argument("--gui", action="store_true", help="the window, prefilled with these arguments")
    args = ap.parse_args()
    if args.gui or not args.file or not args.pos:
        return gui(args)
    setup = Setup(args.file, args.adapter_dir)
    try:
        t = time.time()
        files = render(setup, [parse_positions(p)[0] for p in args.pos],
                       args.exposure_ms or setup.get("exposure-ms", 50), args.frames or 100, args.start_sec,
                       args.out or str(Path.home() / "inSiliScope renders"), find_cli(args.cli))
        print(f"Done: {len(files)} file(s) in {time.time() - t:.1f} s.")
    finally:
        setup.close()


if __name__ == "__main__":
    main()
