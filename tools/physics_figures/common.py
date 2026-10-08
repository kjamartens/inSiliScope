"""Shared machinery of the physics figures (tools/build_physics_figures.py).

Every pixel and number in a figure comes from insiliscope_cli, i.e. from the adapter's Simulation/ render code and the
core: movies (16-bit ADU), and the read-only diagnostic outputs (--photons-out, --psf-out, --splat-out, --setup-json,
--dyes-json, --density-out, --bf-screens-out, --nucleus-json, --presets-json, --geometry-json). This package runs the
cli, reads what it writes and draws it. It computes no physics; the only arithmetic here is on the cli's outputs
(differences, sums, sections, statistics).

A figure is a function registered with @register(id, page): it gets a Ctx, runs the cli through ctx.cli(), writes
images through ctx.tile() (data, no text: one PNG for both themes) and ctx.plot() (matplotlib, a light and a dark
variant), and returns the Markdown that replaces the page's `<!-- fig:<id> -->` marker.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass

import numpy as np
import tifffile
from PIL import Image

import matplotlib

matplotlib.use("Agg")
from matplotlib import colormaps  # noqa: E402
from matplotlib.figure import Figure  # noqa: E402

# One spot of the cell field with a cell, nucleus and microtubules in a 128 px FOV (gallery/manifest.json "overview").
SPOT = {"x": 6.58, "y": -5.61}

DPI = 150
PLOT_W = 7.6          # inches: the content column at DPI is ~1140 px, shown at <= 760 css px
STYLES = {
    "light": {"fg": "#24292f", "muted": "#6e7781", "grid": "#d0d7de", "accent": "#0969da"},
    "dark": {"fg": "#e6edf3", "muted": "#8b949e", "grid": "#3d444d", "accent": "#58a6ff"},
}
# Line colours readable on both themes (Okabe-Ito).
COLORS = ["#E69F00", "#56B4E9", "#009E73", "#D55E00", "#CC79A7", "#0072B2", "#F0E442", "#999999"]


@dataclass
class FigSpec:
    id: str
    page: str
    fn: object
    timed: bool   # renders its timing rows alone (no other figure running), after the others


REGISTRY: list[FigSpec] = []


def register(fid, page, timed=False):
    def deco(fn):
        REGISTRY.append(FigSpec(fid, page, fn, timed))
        return fn
    return deco


# ---- running the cli ---------------------------------------------------------------------------

def fmt(v):
    """An option value as the cli reads it (numbers without float noise)."""
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        return str(int(v)) if v.is_integer() and abs(v) < 1e15 else "%.10g" % v
    if isinstance(v, (list, tuple)):
        return ",".join(fmt(x) for x in v)
    return str(v)


# cli output option (as a keyword) -> file name stem and extension in the figure's temp directory.
OUTPUT_NAMES = {
    "out": ("movie", ".tif"), "photons_out": ("photons", ".tif"), "setup_json": ("setup", ".json"),
    "psf_out": ("psf", ""), "splat_out": ("splat", ".tif"), "dyes_json": ("dyes", ".json"),
    "density_out": ("density", ".tif"), "bf_screens_out": ("bf", ""), "nucleus_json": ("nuclei", ".json"),
    "geometry_json": ("geometry", ".json"), "presets_json": ("presets", ".json"),
}
TIMES_RE = re.compile(r"^(.*?): .*\(setup ([\d.]+) s\), total ([\d.]+) s", re.M)


@dataclass
class Run:
    files: dict        # output keyword -> absolute path
    stdout: str
    wall: float        # s, the whole command (a separate process, or one command of the figure's served cli)
    times: dict        # output path -> (setup s, total s) as the cli printed them
    command: str
    served: bool = False

    def __getitem__(self, key):
        return self.files[key]

    def time(self, key="out"):
        """(setup, total) seconds the cli printed for one output (a movie or --photons-out)."""
        p = self.files[key]
        for path, t in self.times.items():
            if os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(p)):
                return t
        raise KeyError("no time printed for " + key)


class CliError(RuntimeError):
    pass


# Options that change a movie's PSF kernel (its request, or the label state's wavelength it is computed at).
KERNEL_KEYS = ("psf-", "zern.", "na", "immersion-index", "pixel-nm", "mt-dye", "dye1", "dye2", "dye3", "mt-mode",
               "mode", "light-preset", "laser-", "ex-", "dichroic", "em-", "qe", "camera-preset", "modality")
# ... but not these (applied to the kernel after it is computed: not in its fingerprint).
NOT_KERNEL = ("psf-halo-cut", "psf-interp")
# Kernel pools: the PSF kernels most figures share, computed once before the figures run (build_physics_figures.py)
# and hard-linked into a figure's cache directory before a run that needs one (the cli's --disk-cache 2 keeps one
# kernel per directory and replaces it by rename, so a link is never written through).
_WF = {"mt-mode": "WideField", "mt-dye": -1, "light-preset": "auto"}
POOLS = {
    "default": {},
    "dSTORM": {"mt-mode": "dSTORM", "mt-dye": -1, "light-preset": "auto"},
    "PALM": {"mt-mode": "PALM", "mt-dye": -1, "light-preset": "auto"},
    "WideField": _WF,
    # Renderer.Quality's other oversamplings (Fast 4, Exhaustive 8); their compute times feed the oversampling table.
    "os4": {"psf-oversampling": 4},
    "os8": {"psf-oversampling": 8},
    "WideField-os4": dict(_WF, **{"psf-oversampling": 4}),
    "WideField-os8": dict(_WF, **{"psf-oversampling": 8}),
}
POOL_DEFAULTS = {"psf-oversampling": "6"}   # an option at its default value is the same kernel as no option


def pool_of(opts):
    """The kernel pool a run's options match (its kernel-relevant options are exactly a pool's), else None."""
    rel = {k: fmt(v) for k, v in opts.items() if k.startswith(KERNEL_KEYS) and k not in NOT_KERNEL}
    rel = {k: v for k, v in rel.items() if POOL_DEFAULTS.get(k) != v}
    for name, p in POOLS.items():
        if rel == {k: fmt(v) for k, v in p.items()}:
            return name
    return None


def pool_times(cache_root):
    """Each pool's kernel compute time (s) as measured when the build computed it (build_physics_figures.py, alone
    on the machine before the figures): {pool name: seconds}."""
    p = os.path.join(cache_root, "pools.json")
    if not os.path.exists(p):
        return {}
    return {k: v.get("kernel_s") for k, v in load(p).items()}


def link_or_copy(src, dst):
    try:
        if os.path.exists(dst):
            os.remove(dst)
        os.link(src, dst)
    except OSError:
        import shutil
        shutil.copyfile(src, dst)


class Ctx:
    """One figure's working state: its temp directory, cli runs (for the provenance block) and images. Each figure
    has its own cli cache directory (packed cells and the last PSF kernel, --disk-cache 2), seeded from the shared
    one, so figures running side by side never write the same file."""

    def __init__(self, fid, cli, out, cache_root, shared=None):
        self.id = fid
        self.cli_exe = cli
        self.out = out
        self.fig_dir = os.path.join(out, "fig")
        self.tmp = os.path.join(out, "tmp", fid)
        self.cache = os.path.join(cache_root, fid)
        self.cache_root = cache_root
        os.makedirs(self.fig_dir, exist_ok=True)
        os.makedirs(self.tmp, exist_ok=True)
        os.makedirs(self.cache, exist_ok=True)
        if shared:
            blocks = os.path.join(cache_root, shared, "packed_blocks.bin")
            if os.path.exists(blocks):
                import shutil
                shutil.copyfile(blocks, os.path.join(self.cache, "packed_blocks.bin"))
        self.runs: list[Run] = []
        self.env = dict(os.environ, ISC_CACHE_DIR=self.cache)
        self.env.pop("ISC_TIMING", None)
        self.images: list[str] = []
        self._server = None   # the figure's `insiliscope_cli --serve` process (started by the first served run)

    def close(self):
        """End the served cli process, if one was started."""
        srv, self._server = self._server, None
        if srv is None:
            return
        try:
            srv.stdin.close()
            srv.wait(timeout=60)
        except Exception:
            srv.kill()

    def _served(self, args, shown):
        """One command through the figure's served cli (`--serve`: one process keeps the world and PSF kernel
        between commands, outputs byte-identical to separate runs; ctest cli_serve). Returns (exit code, output)."""
        if self._server is None or self._server.poll() is not None:
            self._server = subprocess.Popen([self.cli_exe, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                            env=self.env, cwd=self.tmp)
        srv = self._server
        try:
            srv.stdin.write("\t".join(args) + "\n")
            srv.stdin.flush()
        except OSError as e:
            raise CliError("insiliscope_cli --serve is gone (%s): %s" % (e, " ".join(shown)))
        lines = []
        while True:
            line = srv.stdout.readline()
            if not line:
                self._server = None
                raise CliError("insiliscope_cli --serve ended (exit %s): %s\n%s" %
                               (srv.poll(), " ".join(shown), "".join(lines)[-2000:]))
            if line.startswith("@@isc-done "):
                return int(line.split()[1]), "".join(lines)
            lines.append(line)

    def path(self, name):
        return os.path.join(self.tmp, name)

    def cli(self, opts=None, tag="", fresh=False, **outputs):
        """Run insiliscope_cli with --name value per opts item; each true keyword of OUTPUT_NAMES (out=True,
        photons_out=True, ...) adds that output, written into this figure's temp directory as <tag>.<stem><ext>.
        The figure's commands go to one served cli process; fresh=True runs a separate process instead (for a
        setup time that has to be cold: nothing memoized)."""
        opts = dict(opts or {})
        opts.setdefault("disk-cache", 2)
        pool = pool_of(opts)
        if pool:
            k = os.path.join(self.cache_root, "pool-" + pool, "psf_kernel.bin")
            if os.path.exists(k) and os.path.normcase(os.path.dirname(k)) != os.path.normcase(self.cache):
                link_or_copy(k, os.path.join(self.cache, "psf_kernel.bin"))
        args = [self.cli_exe]
        shown = ["insiliscope_cli"]
        files = {}
        for key, on in outputs.items():
            if not on:
                continue
            short, ext = OUTPUT_NAMES[key]
            p = self.path((tag + "." if tag else "") + short + ext)
            files[key] = p
            args += ["--" + key.replace("_", "-"), p]
            shown += ["--" + key.replace("_", "-"), os.path.basename(p)]
        for k, v in opts.items():
            args += ["--" + k, fmt(v)]
            if k != "disk-cache":
                shown += ["--" + k, fmt(v)]
        t0 = time.perf_counter()
        if fresh:
            r = subprocess.run(args, capture_output=True, text=True, env=self.env)
            code, stdout, err = r.returncode, r.stdout, r.stderr
        else:
            code, stdout = self._served(args[1:], shown)
            err = stdout
        wall = time.perf_counter() - t0
        if code != 0:
            raise CliError("insiliscope_cli failed (%d): %s\n%s" % (code, " ".join(shown), err[-2000:]))
        times = {m.group(1).strip(): (float(m.group(2)), float(m.group(3))) for m in TIMES_RE.finditer(stdout)}
        run = Run(files, stdout, wall, times, " ".join(shown), served=not fresh)
        self.runs.append(run)
        return run

    # ---- images ----

    def _name(self, name, ext="png"):
        fname = "%s-%s.%s" % (self.id, name, ext) if name else "%s.%s" % (self.id, ext)
        self.images.append(fname)
        return fname

    def tile(self, name, arr, cmap="magma", vmin=None, vmax=None, min_px=256, max_px=1024, nan=(0, 0, 0)):
        """A data image (no text): arr (row 0 = top) through a colormap, upscaled by a whole factor to >= min_px
        (pixels stay square blocks). Returns its site path, fig/<file>."""
        a = np.asarray(arr, dtype=np.float64)
        if vmin is None:
            vmin = float(np.nanmin(a))
        if vmax is None:
            vmax = float(np.nanmax(a))
        t = (a - vmin) / (vmax - vmin) if vmax > vmin else np.zeros_like(a)
        rgb = colormaps[cmap](np.clip(np.nan_to_num(t, nan=0.0), 0, 1))[..., :3]
        rgb = (rgb * 255 + 0.5).astype(np.uint8)
        if np.isnan(a).any():
            rgb[np.isnan(a)] = nan
        img = Image.fromarray(rgb, "RGB")
        k = max(1, -(-min_px // max(img.width, img.height)))
        k = min(k, max(1, max_px // max(img.width, img.height)))
        if k > 1:
            img = img.resize((img.width * k, img.height * k), Image.NEAREST)
        fname = self._name(name)
        img.save(os.path.join(self.fig_dir, fname), optimize=True)
        return "fig/" + fname

    def diff_tile(self, name, d, scale, **kw):
        """A difference image, blue (negative) .. white .. red (positive), +/- scale."""
        return self.tile(name, d, cmap="RdBu_r", vmin=-scale, vmax=scale, **kw)

    def gif(self, name, frames, cmap="gray", vmin=None, vmax=None, fps=8, min_px=0, labels=None, levels=64):
        """An animated GIF of data frames (row 0 = top) through a colormap on one scale, at the data's own pixels
        (the page scales it up, pixelated) or upscaled by a whole factor to >= min_px; labels (one per frame,
        optional) are written into the top-left corner. levels: grey levels kept (camera noise compresses badly;
        64 levels make a third of the file). Returns fig/<file>."""
        a = np.asarray(frames, dtype=np.float64)
        vmin = float(np.nanmin(a)) if vmin is None else vmin
        vmax = float(np.nanmax(a)) if vmax is None else vmax
        pal = (colormaps[cmap](np.linspace(0, 1, 256))[:, :3] * 255 + 0.5).astype(np.uint8)
        k = max(1, -(-min_px // max(a.shape[1], a.shape[2])))
        font = None
        if labels:
            from matplotlib import font_manager
            from PIL import ImageFont
            # DejaVu Sans (matplotlib's own) has the micro sign; drawn without anti-aliasing, so it stays crisp when
            # the page scales the image up.
            font = ImageFont.truetype(font_manager.findfont("DejaVu Sans"), max(10, a.shape[1] * k // 10))
        out = []
        for i, f in enumerate(a):
            t = np.clip((f - vmin) / (vmax - vmin) if vmax > vmin else np.zeros_like(f), 0, 1)
            q = np.round(np.round(np.nan_to_num(t) * (levels - 1)) * (255 / (levels - 1))).astype(np.uint8)
            im = Image.fromarray(q, "P")
            im.putpalette(pal.ravel().tolist())
            if k > 1:
                im = im.resize((im.width * k, im.height * k), Image.NEAREST)
            if labels:
                from PIL import ImageDraw
                d = ImageDraw.Draw(im)
                d.fontmode = "1"
                x, y = max(2, im.width // 40), max(1, im.height // 50)
                d.text((x + 1, y + 1), labels[i], fill=0, font=font)
                d.text((x, y), labels[i], fill=255, font=font)
            out.append(im)
        fname = self._name(name, "gif")
        out[0].save(os.path.join(self.fig_dir, fname), save_all=True, append_images=out[1:],
                    duration=int(round(1000 / fps)), loop=0, optimize=False)
        return "fig/" + fname

    def plot(self, name, draw, w=PLOT_W, h=3.0, alt=""):
        """A matplotlib figure in a light and a dark variant: draw(fig, style) builds it. Returns the Markdown of
        both images (Material shows one per theme)."""
        md = []
        for variant, st in STYLES.items():
            rc = {
                "font.size": 8.5, "axes.titlesize": 9, "axes.labelsize": 8.5, "legend.fontsize": 7.5,
                "xtick.labelsize": 7.5, "ytick.labelsize": 7.5,
                "text.color": st["fg"], "axes.labelcolor": st["fg"], "axes.edgecolor": st["muted"],
                "xtick.color": st["muted"], "ytick.color": st["muted"], "axes.titlecolor": st["fg"],
                "axes.facecolor": "none", "figure.facecolor": "none", "savefig.facecolor": "none",
                "grid.color": st["grid"], "grid.linewidth": 0.6, "legend.frameon": False,
                "legend.labelcolor": st["fg"], "axes.spines.top": False, "axes.spines.right": False,
                "image.interpolation": "nearest", "axes.prop_cycle": matplotlib.cycler(color=COLORS),
            }
            with matplotlib.rc_context(rc):
                fig = Figure(figsize=(w, h), dpi=DPI, layout="constrained")
                draw(fig, st)
                fname = self._name("%s.%s" % (name, variant) if name else variant)
                fig.savefig(os.path.join(self.fig_dir, fname), transparent=True, bbox_inches="tight", pad_inches=0.04)
            md.append("![%s](fig/%s#only-%s)" % (alt, fname, variant))
        return "\n".join(md)


# ---- reading the cli's outputs -------------------------------------------------------------------

def stack(path):
    """A TIFF as (pages, rows, cols), float64."""
    a = tifffile.imread(path)
    if a.ndim == 2:
        a = a[None]
    return a.astype(np.float64)


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def rows(flat, stride):
    """A flat JSON array as (n, stride) floats; null (the cli writes infinities as null) becomes inf."""
    a = np.asarray([np.inf if v is None else v for v in flat], dtype=np.float64)
    return a.reshape(-1, stride) if a.size else np.zeros((0, stride))


def read_drift(movie_path):
    """<movie>.drift.csv (frame, dx, dy, dz in nm) as an array, or None."""
    p = os.path.splitext(movie_path)[0] + ".drift.csv"
    if not os.path.exists(p):
        return None
    return np.loadtxt(p, delimiter=",", skiprows=1, ndmin=2)


# ---- numbers -------------------------------------------------------------------------------------

def sig(x, n=2, tex=False):
    """x to n significant digits, without exponent noise for ordinary numbers (HTML, or matplotlib mathtext)."""
    if x is None or not np.isfinite(x):
        return "-"
    if x == 0:
        return "0"
    a = abs(x)
    if a >= 1e5 or a < 1e-3:
        m, e = ("%.*e" % (n - 1, x)).split("e")
        return (r"$%s\times10^{%d}$" if tex else "%s&times;10<sup>%d</sup>") % (m, int(e))
    d = max(0, n - 1 - int(np.floor(np.log10(a))))
    return "%.*f" % (d, x)


def secs(t):
    """A duration in the unit that suits it, to 2-3 significant digits: 12.3 s, 1.23 s, 35 ms, 4.1 ms, 240 µs. The
    cli prints its times to 0.1 ms; per-frame and per-blink times are divided over many frames or blinks."""
    if t is None or not np.isfinite(t):
        return "-"
    if t <= 0:
        return "< 0.1 ms"
    if t >= 100:
        return "%.0f s" % t
    if t >= 1:
        return "%s s" % sig(t, 3)
    if t >= 1e-3:
        return "%s ms" % sig(t * 1e3, 2 if t < 0.1 else 3)
    return "%s µs" % sig(t * 1e6, 2)


def robust_range(a, lo=0.5, hi=99.8):
    a = np.asarray(a)
    return float(np.percentile(a, lo)), float(np.percentile(a, hi))


# ---- Markdown ------------------------------------------------------------------------------------

def img(path, alt="", cls="isc-px"):
    return "![%s](%s){ .%s }" % (alt, path, cls) if cls else "![%s](%s)" % (alt, path)


def figure(body, caption, max_width=None):
    """A figure with a caption (md_in_html); max_width (css, e.g. "34rem") for a figure narrower than the column."""
    style = ' style="max-width: %s"' % max_width if max_width else ""
    # md_in_html parses a nested element only when it has its own markdown attribute (code spans, links, emphasis).
    return '<figure markdown="span" class="isc-fig"%s>\n%s\n<figcaption markdown="span">%s</figcaption>\n</figure>\n' % (
        style, body, caption)


def table(header, body_rows, cls="isc-cmp", label_col=True):
    """A Markdown table; cells are Markdown (images allowed). Wrapped so the CSS can size its images: --isc-n (the
    image columns) lets it share the width between them. label_col: the first column holds row labels (small,
    narrow); False: every column is an image column."""
    def row(cells):
        return "| " + " | ".join(str(c).replace("|", "&#124;") for c in cells) + " |"   # a pipe would split a cell
    n = len(header) - 1 if label_col else len(header)
    if not label_col:
        cls += " isc-nolabel"
    out = ['<div class="%s" style="--isc-n: %d" markdown>' % (cls, max(1, n)), "", row(header),
           "|" + "|".join([":--" if label_col else ":-:"] + [":-:"] * (len(header) - 1)) + "|"]
    out += [row(r) for r in body_rows]
    out += ["", "</div>", ""]
    return "\n".join(out)


def provenance(ctx, extra=""):
    """The collapsed 'how this was made' block: every cli command of the figure and its time."""
    total = sum(r.wall for r in ctx.runs)
    lines = ['??? note "How this figure was made (%d cli run%s, %s)"' % (len(ctx.runs), "" if len(ctx.runs) == 1 else "s",
                                                                      secs(total)),
             "    Generated by `tools/build_physics_figures.py` (figure `%s`) from the code it documents%s. "
             "Commands, in order, with their wall time%s:" %
             (ctx.id, BUILD_INFO.get("where", ""),
              "" if not any(r.served for r in ctx.runs) else
              " (those marked `[served]` ran one after the other in one `insiliscope_cli --serve` process, which "
              "keeps the world and the PSF kernel between commands; their outputs are the same as separate runs')"),
             ""]
    if extra:
        lines[2:2] = ["    " + extra, ""]
    lines.append("    ```text")
    for r in ctx.runs:
        lines.append("    %s    # %s%s" % (r.command, secs(r.wall), " [served]" if r.served else ""))
    lines.append("    ```")
    return "\n".join(lines) + "\n"


BUILD_INFO: dict = {}
