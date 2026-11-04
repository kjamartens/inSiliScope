# SPDX-License-Identifier: BSD-3-Clause
"""Ground truth page (docs/ground-truth.md): what a blink with so many photons in its core looks like in the movie."""
import csv
import os

import numpy as np

from .common import figure, provenance, register, stack

LEVELS = [50, 100, 200, 300, 500, 1000, 3000]   # the rows: blinks with core photons within 15 % of each
COLS = 3                              # examples per row and condition
HALF = 7                              # crop half-width (px): 15 x 15 px
ISOLATED = 6                          # px: no other blink of >= 30 core photons nearer than this in its frame
PX_NM = 100.0                         # the cli's default pixel (x [nm] = (pixel + 0.5) x PX_NM)
GOOD_CAMERA = {"read-noise": 0.5}
NO_BACKGROUND = {"illum-bfp": 0, "chamber-height-um": 0.001}   # epi; the free imager's column next to nothing
CONDITIONS = [
    ("Default", "HILO, imager background, 1.2 e⁻ read noise", {}),
    ("Better camera", "the same, 0.5 e⁻ read noise", GOOD_CAMERA),
    ("No background", "epi, no imager in solution, 1.2 e⁻", NO_BACKGROUND),
    ("No background, better camera", "epi, no imager, 0.5 e⁻", dict(NO_BACKGROUND, **GOOD_CAMERA)),
]


def blinks(ctx, tag, opts):
    """One condition's movie and its ground truth (every blink drawn, --gt-min 0) as arrays."""
    gt = tag + ".gt"
    os.makedirs(ctx.path(gt), exist_ok=True)
    r = ctx.cli(dict({"size": 128, "frames": 100, "gt-out": gt, "gt-min": 0}, **opts), tag=tag, out=True)
    with open(ctx.path(os.path.join(gt, "localizations.csv")), encoding="utf-8") as f:
        rows = [x for x in csv.DictReader(f) if x["kind"] == "emitter"]
    col = lambda k: np.array([float(x[k]) for x in rows])   # noqa: E731
    return dict(movie=stack(r["out"]), frame=col("frame").astype(int), x=col("x [nm]") / PX_NM - 0.5,
                y=col("y [nm]") / PX_NM - 0.5, core=col("core_photons"), z=col("z [nm]"), photons=col("intensity [photon]"),
                bg=col("offset [photon]"))


def examples(b, rng):
    """Per level, COLS blinks with their crop inside the image and no other visible blink near them."""
    T, H, W = b["movie"].shape
    x, y, fr, core = b["x"], b["y"], b["frame"], b["core"]
    inside = (x >= HALF) & (y >= HALF) & (x < W - 1 - HALF) & (y < H - 1 - HALF)
    lone = np.ones(len(x), bool)
    for t in np.unique(fr):
        k = np.flatnonzero(fr == t)
        o = k[core[k] >= 30]
        for i in k:
            near = (x[o] - x[i]) ** 2 + (y[o] - y[i]) ** 2 < ISOLATED ** 2
            lone[i] = not np.any(near & (o != i))
    out = []
    for s in LEVELS:
        cand = np.flatnonzero(inside & lone & (core >= s / 1.15) & (core <= s * 1.15))
        sel = rng.choice(cand, size=min(COLS, len(cand)), replace=False) if len(cand) else []
        out.append(sorted(sel, key=lambda i: abs(b["z"][i])))
    return out


def crop(b, i):
    """The 15 x 15 px around blink i in its frame, and its grey scale: from 2.5 noise sigmas below the tile's border
    median to 6 above (the border's spread, robustly; at least 1 ADU), so equal noise looks the same in every tile."""
    cx, cy = int(round(b["x"][i])), int(round(b["y"][i]))
    c = b["movie"][b["frame"][i], cy - HALF:cy + HALF + 1, cx - HALF:cx + HALF + 1]
    ring = np.concatenate([c[0], c[-1], c[1:-1, 0], c[1:-1, -1]])
    med = float(np.median(ring))
    sd = max(1.0, 1.4826 * float(np.median(np.abs(ring - med))))
    return c, med - 2.5 * sd, med + 6 * sd


@register("gt-core", "ground-truth")
def gt_core(ctx):
    # The default sample and microscope (DNA-PAINT microtubules, ATTO 655 at 640 nm), 100 frames of 50 ms, under four
    # conditions; every blink drawn kept in the ground truth with its core photons.
    data = [blinks(ctx, "c%d" % k, opts) for k, (_, _, opts) in enumerate(CONDITIONS)]
    picks = [examples(b, np.random.default_rng(1)) for b in data]

    def draw(fig, st):
        subs = fig.subfigures(1, len(CONDITIONS), wspace=0.02, width_ratios=[1.3] + [1] * (len(CONDITIONS) - 1))
        for gi, (sub, (title, sub_title, _), b, pk) in enumerate(zip(subs, CONDITIONS, data, picks)):
            sub.suptitle("%s\n%s" % (title, sub_title), fontsize=7, color=st["fg"])
            axs = sub.subplots(len(LEVELS), COLS, gridspec_kw={"wspace": 0.05, "hspace": 0.28})
            for ri, sel in enumerate(pk):
                for ci in range(COLS):
                    ax = axs[ri, ci]
                    ax.set_xticks([])
                    ax.set_yticks([])
                    for sp in ax.spines.values():
                        sp.set_visible(False)
                    if ci >= len(sel):
                        continue
                    i = sel[ci]
                    c, lo, hi = crop(b, i)
                    n = 2 * HALF + 1
                    ax.imshow(c, cmap="gray", vmin=lo, vmax=hi, interpolation="nearest")
                    for a in ((-0.5, 1.5), (n - 2.5, n - 0.5)):
                        ax.plot(a, (HALF, HALF), color=st["accent"], lw=0.9)
                        ax.plot((HALF, HALF), a, color=st["accent"], lw=0.9)
                    ax.set_xlim(-0.5, n - 0.5)
                    ax.set_ylim(n - 0.5, -0.5)
                    ax.set_title("%.0f, %+.1f" % (b["photons"][i], b["z"][i] / 1000), fontsize=5,
                                 color=st["muted"], pad=1.2)
                if gi == 0:
                    axs[ri, 0].set_ylabel("%g" % LEVELS[ri], rotation=0, ha="right", va="center", fontsize=7.5,
                                          color=st["fg"])

    counts = "; ".join("%s: %s" % (t, ", ".join("%d" % int(np.sum(b["core"] >= s)) for s in (100, 300, 1000)))
                       for (t, _, _), b in zip(CONDITIONS, data))
    bgs = ", ".join("%s %.3g" % (t.lower(), float(np.nanmedian(b["bg"]))) for (t, _, _), b in zip(CONDITIONS, data))
    body = ctx.plot("", draw, h=6.4, alt="Movie crops of blinks with 50 to 3000 photons in their core under four conditions")
    return figure(body + "\n" + provenance(ctx),
                  "What a blink with so many photons in its core looks like: the threshold of localizations.csv. The "
                  "default sample (DNA-PAINT microtubules, ATTO 655, 100 nm pixels, 50 ms frames) under four conditions: "
                  "the default microscope (HILO, with the free imager's background), the same with a camera of 0.5 e⁻ "
                  "read noise, epi with no imager in solution (no background), and that with the better camera. A row "
                  "holds blinks within 15 %% of its core photons: the expected photons landing in the 500 x 500 nm "
                  "square centred on the blink, from the PSF at its own sampling (the same at any pixel size). Each tile "
                  "is 15 x 15 pixels of one frame, centred on a blink (the ticks) with no other blink of 30 or more core "
                  "photons within 0.6 µm, scaled from 2.5 noise sigmas below its own background to 6 above, so equal "
                  "noise looks the same everywhere; above it, its photons on the camera and its height above the focal "
                  "plane (µm): a blink far out of focus has many more photons than its core. Median background under a "
                  "blink (photons per pixel): %s. Blinks in these 100 frames with at least 100, 300 (the default "
                  "threshold) and 1000 core photons: %s." % (bgs, counts))
