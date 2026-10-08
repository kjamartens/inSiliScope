"""World model page: the chunk grid and packing, the cell outline's lobes and fractal edge."""
import numpy as np
from PIL import Image, ImageDraw

from .common import COLORS, SPOT, figure, load, register, sig


def geometry(ctx, size_um, tag, detail=False, **opts):
    r = ctx.cli(dict(SPOT, **{"geometry-um": size_um, "geometry-detail": 1 if detail else 0}, **opts), tag=tag,
                geometry_json=True)
    return load(r["geometry_json"])


def overlap_fraction(geo, step=0.25):
    """Share of the covered area that lies in two or more cells (outlines rasterised at step um)."""
    n = int(round((geo["x1"] - geo["x0"]) / step))
    count = np.zeros((n, n), np.int32)
    for c in geo["cells"]:
        im = Image.new("L", (n, n), 0)
        ImageDraw.Draw(im).polygon([((x - geo["x0"]) / step, (y - geo["y0"]) / step) for x, y in c["outline"]], fill=1)
        count += np.asarray(im, np.int32)
    covered = (count > 0).sum()
    return float((count > 1).sum() / covered) if covered else 0.0


def polygon_area(pts):
    p = np.asarray(pts)
    return 0.5 * abs(np.dot(p[:, 0], np.roll(p[:, 1], 1)) - np.dot(p[:, 1], np.roll(p[:, 0], 1)))


def nearest_cell(geo, x, y):
    return min(geo["cells"], key=lambda c: (c["x"] - x) ** 2 + (c["y"] - y) ** 2)


@register("world-packing", "world-model")
def world_packing(ctx):
    size = 130.0
    occ = 0.5   # denser than the default 0.33, so that candidates overlap often (gallery world-dense-cells)
    loose = geometry(ctx, size, "packing0", packing=0, occupancy=occ)
    packed = geometry(ctx, size, "packing1", occupancy=occ)
    chunk = 26.0
    f0, f1 = overlap_fraction(loose), overlap_fraction(packed)
    cand = np.array([[c["x"], c["y"]] for c in loose["cells"]])
    new = np.array([[c["x"], c["y"]] for c in packed["cells"]])
    # A candidate is kept if a packed cell centre lies within 8 um of it (packing moves cells by a few um).
    kept = np.array([np.min(np.hypot(*(new - p).T)) < 8 for p in cand]) if len(new) else np.zeros(len(cand), bool)

    def draw(fig, st):
        axs = fig.subplots(1, 2)
        for ax, geo, title in ((axs[0], loose, "Candidates (packing 0)"), (axs[1], packed, "Packed (packing 1)")):
            x0, y0, x1, y1 = geo["x0"], geo["y0"], geo["x1"], geo["y1"]
            for g in np.arange(np.ceil(x0 / chunk) * chunk, x1, chunk):
                ax.axvline(g, color=st["grid"], lw=0.6, zorder=0)
            for g in np.arange(np.ceil(y0 / chunk) * chunk, y1, chunk):
                ax.axhline(g, color=st["grid"], lw=0.6, zorder=0)
            for c in geo["cells"]:
                o = np.array(c["outline"] + c["outline"][:1])
                ax.fill(o[:, 0], o[:, 1], color=COLORS[1], alpha=0.35, lw=0)
                ax.plot(o[:, 0], o[:, 1], color=COLORS[1], lw=0.7)
            ax.plot(cand[:, 0], cand[:, 1], "o", ms=2.2, color=st["fg"], label="Candidate centres")
            if geo is packed:
                ax.plot(new[:, 0], new[:, 1], "x", ms=3.5, color=COLORS[3], label="Packed centres")
                ax.plot(cand[~kept, 0], cand[~kept, 1], "o", ms=7, mfc="none", color=COLORS[3], lw=0.8,
                        label="Removed (still overlapping)")
            ax.set_xlim(x0, x1)
            ax.set_ylim(y1, y0)  # row 0 at the top, as in the camera images
            ax.set_aspect("equal")
            ax.set_title(title)
            ax.set_xlabel("x (µm)")
        axs[0].set_ylabel("y (µm)")
        h0, l0 = axs[1].get_legend_handles_labels()
        fig.legend(h0, l0, loc="lower center", ncol=3, bbox_to_anchor=(0.5, -0.04))

    body = ctx.plot("", draw, h=3.9, alt="Cells before and after packing")
    return figure(body,
                  "Each 26 µm chunk (grey grid) holds at most one candidate cell centre (dots). Left: the candidates "
                  "as drawn, overlapping (darker blue): **%s %%** of the covered area lies in two cells. Right: "
                  "packing moves overlapping neighbours apart (x: new centres) and removes those still stuck after its "
                  "fixed number of iterations (circled; %d of %d candidates kept): **%s %%** overlap. Cell sizes "
                  "never change. Seed 42, occupancy %g (default 0.33), a %g µm square around (%g, %g) µm."
                  % (sig(100 * f0, 2), int(kept.sum()), len(cand), sig(100 * f1, 2), occ, size, SPOT["x"], SPOT["y"]))


@register("world-outline", "world-model")
def world_outline(ctx):
    steps = [
        ("ellipse", {"p.cellBlob": 0, "p.cellRough": 0}, "Ellipse|cellBlob 0"),
        ("lobes", {"p.cellRough": 0}, "+ Lobes|cellBlob 1.75, cellRough 0"),
        ("fractal105", {"p.cellFractalDim": 1.05}, "+ Fractal edge|D = 1.05"),
        ("fractal", {}, "D = 1.35|(default)"),
        ("fractal18", {"p.cellFractalDim": 1.8}, "D = 1.8"),
    ]
    cells = []
    for tag, opts, title in steps:
        geo = geometry(ctx, 60, tag, packing=0, **opts)
        cells.append((nearest_cell(geo, SPOT["x"], SPOT["y"]), title))
    cx, cy = cells[0][0]["x"], cells[0][0]["y"]
    diam = [2 * np.sqrt(polygon_area(c["outline"]) / np.pi) for c, _ in cells]
    half = max(np.abs(np.array(c["outline"]) - [cx, cy]).max() for c, _ in cells) * 1.05

    def draw(fig, st):
        axs = fig.subplots(1, len(cells))
        for ax, (c, title) in zip(axs, cells):
            o = np.array(c["outline"] + c["outline"][:1])
            ax.fill(o[:, 0] - cx, o[:, 1] - cy, color=COLORS[1], alpha=0.25, lw=0)
            ax.plot(o[:, 0] - cx, o[:, 1] - cy, color=COLORS[1], lw=0.8)
            ax.set_xlim(-half, half)
            ax.set_ylim(half, -half)
            ax.set_aspect("equal")
            ax.set_title(title.replace("|", chr(10)), fontsize=7.5)
            ax.set_xticks([-10, 0, 10])
            ax.set_yticks([-10, 0, 10])
            ax.set_xlabel("µm")

    body = ctx.plot("", draw, h=2.2, alt="One cell outline step by step")
    return figure(body,
                  "One cell's outline as the parameters are switched on (packing off, so the cell stays put): an "
                  "ellipse, then the angular harmonics (lobes), then the fractal tail of harmonics 6-64 with "
                  "amplitude k<sup>-(2.5-D)</sup>; a larger box-counting dimension D roughens the edge. Area-equivalent "
                  "diameters: %s µm." % " / ".join("%.1f" % d for d in diam))
