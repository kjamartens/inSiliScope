# SPDX-License-Identifier: BSD-3-Clause
"""Ground truth page (docs/ground-truth.md): what a blink with so many photons in its core looks like in the movie."""
import csv
import os

import numpy as np
from matplotlib import patheffects
from matplotlib.patches import Rectangle

from .common import figure, provenance, register, stack

LEVELS = [30, 50, 100, 200, 300, 500, 1000, 2000]   # the columns: photons in the core
DEFAULT_LEVEL = 300                                   # localizations.csv's default threshold
TOL = (1.08, 1.2)        # a blink within 8 % of its level, else within 20 %
FOCUS_NM = 250           # in focus: |z| below this
HALF = 7                 # crop half-width (px): 15 x 15 px
PX_NM = 100.0            # the cli's default pixel (x [nm] = (pixel + 0.5) x PX_NM)
CORE_NM = 500.0          # the core's side (GroundTruth::kCoreBoxNm)
FRAMES = 600
LABEL_PCT = 5            # the microtubules' labelling: sparse blinks; the background (the free imager) is the same
GOOD_CAMERA = {"read-noise": 0.5}
NO_BACKGROUND = {"illum-bfp": 0, "chamber-height-um": 0.001}   # epi; the free imager's column next to nothing
OUTLINE = [patheffects.withStroke(linewidth=1.2, foreground="#000000")]   # light text on any pixel
CONDITIONS = [
    ("Default", "HILO, imager in solution,\n1.2 e⁻ read noise", {}),
    ("Better camera", "the same,\n0.5 e⁻ read noise", GOOD_CAMERA),
    ("No background", "epi, no imager in solution,\n1.2 e⁻ read noise", NO_BACKGROUND),
    ("Both", "epi, no imager,\n0.5 e⁻ read noise", dict(NO_BACKGROUND, **GOOD_CAMERA)),
]


def blinks(ctx, tag, opts):
    """One condition's movie and its ground truth (every blink drawn, --gt-min 0) as arrays."""
    gt = tag + ".gt"
    os.makedirs(ctx.path(gt), exist_ok=True)
    r = ctx.cli(dict({"size": 128, "frames": FRAMES, "mt-label-pct": LABEL_PCT, "gt-out": gt, "gt-min": 0}, **opts),
                tag=tag, out=True)
    with open(ctx.path(os.path.join(gt, "localizations.csv")), encoding="utf-8") as f:
        rows = [x for x in csv.DictReader(f) if x["kind"] == "emitter"]
    col = lambda k: np.array([float(x[k]) for x in rows])   # noqa: E731
    return dict(movie=stack(r["out"]), frame=col("frame").astype(int), x=col("x [nm]") / PX_NM - 0.5,
                y=col("y [nm]") / PX_NM - 0.5, core=col("core_photons"), z=col("z [nm]"),
                photons=col("intensity [photon]"), bg=col("offset [photon]"))


def alone(b):
    """Per blink: no other blink's light near its tile (none with 5 or more core photons within the tile and 3 px
    around it, none with 100 or more photons on the camera within twice the tile: a defocused one's wide disc)."""
    x, y, fr, core, ph = b["x"], b["y"], b["frame"], b["core"], b["photons"]
    out = np.ones(len(x), bool)
    for t in np.unique(fr):
        k = np.flatnonzero(fr == t)
        for i in k:
            o = k[k != i]
            dx, dy = np.abs(x[o] - x[i]), np.abs(y[o] - y[i])
            near = (dx <= HALF + 3) & (dy <= HALF + 3) & (core[o] >= 5)
            wide = (dx <= 2 * HALF) & (dy <= 2 * HALF) & (ph[o] >= 100)
            out[i] = not np.any(near | wide)
    return out


def pick(b):
    """Per level the in-focus, lone blink nearest it with its tile inside the image (None if there is none)."""
    T, H, W = b["movie"].shape
    x, y, core, z = b["x"], b["y"], b["core"], b["z"]
    ok = (x >= HALF) & (y >= HALF) & (x < W - 1 - HALF) & (y < H - 1 - HALF) & (np.abs(z) < FOCUS_NM) & alone(b)
    out = []
    for s in LEVELS:
        sel = None
        for tol in TOL:
            cand = np.flatnonzero(ok & (core >= s / tol) & (core <= s * tol))
            if len(cand):
                sel = int(cand[np.argmin(np.abs(np.log(core[cand] / s)))])
                break
        out.append(sel)
    return out


def crop(b, i):
    """The 15 x 15 px around blink i in its frame, and its grey scale: from 2.5 noise sigmas below the tile's border
    median (the border's spread, robustly; at least 1 ADU) to 6 above it or the tile's brightest pixel if higher, so
    equal noise looks the same in the dim tiles and a bright blink keeps its shape."""
    cx, cy = int(round(b["x"][i])), int(round(b["y"][i]))
    c = b["movie"][b["frame"][i], cy - HALF:cy + HALF + 1, cx - HALF:cx + HALF + 1].astype(float)
    ring = np.concatenate([c[0], c[-1], c[1:-1, 0], c[1:-1, -1]])
    med = float(np.median(ring))
    sd = max(1.0, 1.4826 * float(np.median(np.abs(ring - med))))
    return c, med - 2.5 * sd, max(med + 6 * sd, float(c.max())), cx, cy


@register("gt-core", "ground-truth")
def gt_core(ctx):
    # The default sample and microscope (DNA-PAINT microtubules, ATTO 655 at 640 nm), the labelling lowered so blinks
    # stand alone, under four conditions; every blink drawn kept in the ground truth with its core photons.
    data = [blinks(ctx, "c%d" % k, opts) for k, (_, _, opts) in enumerate(CONDITIONS)]
    picks = [pick(b) for b in data]
    n = 2 * HALF + 1
    side = CORE_NM / PX_NM

    def draw(fig, st):
        fig.set_layout_engine("none")   # the grid below sets the spacing (square tiles, rows close together)
        gs = fig.add_gridspec(len(CONDITIONS), len(LEVELS), left=0.2, right=0.995, top=0.88, bottom=0.0,
                              wspace=0.06, hspace=0.1)
        fig.text(0.2 + (0.995 - 0.2) / 2, 0.985, "expected photons in the blink's 500 × 500 nm core", ha="center",
                 va="top", fontsize=8.5, color=st["fg"])
        for ri, ((title, sub, _), b, pk) in enumerate(zip(CONDITIONS, data, picks)):
            for ci, i in enumerate(pk):
                ax = fig.add_subplot(gs[ri, ci])
                ax.set_xticks([])
                ax.set_yticks([])
                hot = LEVELS[ci] == DEFAULT_LEVEL
                for sp in ax.spines.values():
                    sp.set_visible(hot)
                    sp.set_color(st["accent"])
                    sp.set_linewidth(1.4)
                ax.set_xlim(-0.5, n - 0.5)
                ax.set_ylim(n - 0.5, -0.5)
                if ri == 0:
                    ax.set_title("%g%s" % (LEVELS[ci], "  (default)" if hot else ""), fontsize=7.5, pad=3,
                                 color=st["accent"] if hot else st["fg"], fontweight="bold" if hot else "normal")
                if ci == 0:
                    ax.set_ylabel("%s\n%s" % (title, sub), rotation=0, ha="right", va="center", fontsize=7,
                                  color=st["fg"], labelpad=6)
                if i is None:
                    ax.text(HALF, HALF, "none in\nfocus", ha="center", va="center", fontsize=6, color=st["muted"])
                    continue
                c, lo, hi, cx, cy = crop(b, i)
                ax.imshow(c, cmap="gray", vmin=lo, vmax=hi, interpolation="nearest")
                # the core: the 500 nm square centred on the blink's true position
                bx, by = b["x"][i] - cx + HALF, b["y"][i] - cy + HALF
                ax.add_patch(Rectangle((bx - side / 2, by - side / 2), side, side, fill=False, lw=0.6, ls=(0, (2, 2)),
                                       ec=st["accent"], alpha=0.9))
                ax.text(n - 1.2, n - 1.0, "%.0f" % b["core"][i], ha="right", va="bottom", fontsize=5, color="#f0f0f0",
                        path_effects=OUTLINE)
                if ri == len(CONDITIONS) - 1 and ci == 0:   # 1 µm scale bar
                    ax.plot((0.8, 0.8 + 1000 / PX_NM), (n - 1.6, n - 1.6), color="#f0f0f0", lw=1.5,
                            solid_capstyle="butt")
                    ax.text(0.8 + 500 / PX_NM, n - 2.2, "1 µm", ha="center", va="bottom", fontsize=5, color="#f0f0f0",
                            path_effects=OUTLINE)

    bgs = ", ".join("%s %.3g" % (t.lower(), float(np.nanmedian(b["bg"]))) for (t, _, _), b in zip(CONDITIONS, data))
    body = ctx.plot("", draw, h=3.45, alt="Movie crops of lone, in-focus blinks with 30 to 2000 photons in their core "
                                         "under four conditions")
    return figure(body + "\n" + provenance(ctx),
                  "Blinks by the photons in their 500 nm core (dashed; the number in the corner). Each tile: 15 x 15 "
                  "pixels of one frame, a lone blink within %d nm of focus (DNA-PAINT microtubules, labelled at %d %% so "
                  "blinks stand alone), scaled to its own noise. %d (marked) is the default threshold. Rows: the "
                  "default microscope; a 0.5 e⁻ read-noise camera; epi without imager background; both. Median "
                  "background (photons/pixel): %s." % (FOCUS_NM, LABEL_PCT, DEFAULT_LEVEL, bgs))
