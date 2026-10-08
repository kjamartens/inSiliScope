"""Structures page: the shaped nucleus, the relaxed cytoplasm height, microtubule paths and the dye lattice."""
import numpy as np
from PIL import Image, ImageDraw

from matplotlib.collections import LineCollection

from .common import COLORS, SPOT, figure, load, register, rows, sig

PLAIN_NUCLEUS = {"p.nucIrregMin": 0, "p.nucIrregMax": 0, "p.nucBendMax": 0, "p.nucThickIrreg": 0, "p.nucAsym": 0,
                 "p.nucWidestMin": 0.5, "p.nucWidestMax": 0.5}


def cells_near(geo, x, y, n):
    return sorted(geo["cells"], key=lambda c: (c["x"] - x) ** 2 + (c["y"] - y) ** 2)[:n]


def match(cells, x, y):
    return min(cells, key=lambda c: (c["x"] - x) ** 2 + (c["y"] - y) ** 2)


def rings_of(nuc_cell, slices, pts):
    return np.asarray(nuc_cell["rings"], float).reshape(slices, pts, 3)


def widest(rg):
    k = int(np.argmax([np.ptp(r[:, 0]) * np.ptp(r[:, 1]) for r in rg]))
    return np.vstack([rg[k], rg[k][:1]])


def silhouette(rg, c0, u):
    """The nucleus seen from the side along u: (along, height) of a closed outline."""
    us = (rg[:, :, :2] - c0) @ u
    z = rg[:, :, 2].mean(axis=1)
    return np.r_[us.min(1), us.max(1)[::-1], us.min(1)[:1]], np.r_[z, z[::-1], z[:1]]


def height_raster(cell, step):
    """The cytoplasm mesh of one cell rasterised (each quad at its mean height): (image, x0, y0)."""
    m = cell["mesh"]
    R, n, v = m["rings"], m["n"], np.asarray(m["v"], float)
    x0, y0 = v[:, 0].min() - step, v[:, 1].min() - step
    W = int(np.ceil((v[:, 0].max() - x0) / step)) + 2
    H = int(np.ceil((v[:, 1].max() - y0) / step)) + 2
    im = Image.new("F", (W, H), 0.0)
    d = ImageDraw.Draw(im)
    for k in range(R):
        for i in range(n):
            j = (i + 1) % n
            q = v[[k * n + i, k * n + j, (k + 1) * n + j, (k + 1) * n + i]]
            d.polygon([((p[0] - x0) / step, (p[1] - y0) / step) for p in q], fill=float(q[:, 2].mean()))
    return np.asarray(im, float), x0, y0


def sample_line(img, x0, y0, step, p0, p1, n=600):
    t = np.linspace(0, 1, n)
    xs, ys = p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t
    ix = np.clip(((xs - x0) / step).astype(int), 0, img.shape[1] - 1)
    iy = np.clip(((ys - y0) / step).astype(int), 0, img.shape[0] - 1)
    return t * np.hypot(p1[0] - p0[0], p1[1] - p0[1]), img[iy, ix]


def axis_of(nucleus):
    return np.array([np.cos(nucleus["rot"]), np.sin(nucleus["rot"])])


def cell_with_nucleus(ctx, tag, opts, n=1, size=2, detail=True):
    """The n cells nearest the spot (those that overlap a square of side size um around it), each with its nucleus
    rings: [(cell, rings)]."""
    r = ctx.cli(dict(SPOT, **{"geometry-um": size, "geometry-detail": 1 if detail else 0}, **opts), tag=tag,
                geometry_json=True, nucleus_json=True)
    geo, nuc = load(r["geometry_json"]), load(r["nucleus_json"])
    return [(c, rings_of(match(nuc["cells"], c["x"], c["y"]), nuc["slices"], nuc["pts"]))
            for c in cells_near(geo, SPOT["x"], SPOT["y"], n)]


@register("struct-nucleus", "structures")
def struct_nucleus(ctx):
    shaped = cell_with_nucleus(ctx, "shaped", {}, n=4, size=70, detail=False)
    plain = cell_with_nucleus(ctx, "plain", PLAIN_NUCLEUS, n=4, size=70, detail=False)

    def draw(fig, st):
        axs = fig.subplots(2, len(shaped), gridspec_kw={"height_ratios": [1.6, 1]})
        for col, (cell, rg) in enumerate(shaped):
            nucleus = cell["nucleus"]
            u = axis_of(nucleus)
            w = np.array([-u[1], u[0]])
            c0 = np.array([nucleus["x"], nucleus["y"]])
            rgp = min(plain, key=lambda cr: (cr[0]["x"] - cell["x"]) ** 2 + (cr[0]["y"] - cell["y"]) ** 2)[1]
            for k in range(1, len(rg) - 1, 3):
                rr = np.vstack([rg[k], rg[k][:1]])
                axs[0, col].plot((rr[:, :2] - c0) @ u, (rr[:, :2] - c0) @ w, color=COLORS[4], lw=0.4, alpha=0.6)
            for g, style, label in ((rgp, dict(color=st["muted"], ls="--", lw=0.9), "plain ellipsoid"),
                                    (rg, dict(color=COLORS[4], lw=1.2), "shaped (default)")):
                ring = widest(g)
                axs[0, col].plot((ring[:, :2] - c0) @ u, (ring[:, :2] - c0) @ w, label=label, **style)
                axs[1, col].plot(*silhouette(g, c0, u), **style)
            axs[0, col].set_aspect("equal")
            axs[0, col].set_title("cell at (%.0f, %.0f) um" % (cell["x"], cell["y"]), fontsize=7.5)
            axs[1, col].axhline(0, color=st["muted"], lw=0.8)
            axs[1, col].set_aspect("equal")
            axs[1, col].set_ylim(-0.4, None)
            axs[1, col].set_xlabel("along the long axis (um)", fontsize=7)
        axs[0, 0].set_ylabel("top view (um)")
        axs[1, 0].set_ylabel("height (um)")
        h, l = axs[0, 0].get_legend_handles_labels()
        fig.legend(h, l, loc="outside upper center", ncol=2)

    body = ctx.plot("", draw, h=3.7, alt="Shaped and plain nuclei, top and side views")
    return figure(body,
                  "The nuclei of the four cells nearest the figures' spot (seed 42). Top: in each nucleus's own frame "
                  "(long axis horizontal), the widest section and, faint, every third section. Bottom: the "
                  "silhouette seen from the side. Pink: the shaped nucleus (defaults: lobes, kidney bend, uneven "
                  "thickness, wider base, lowered widest point); dashed: the same cell with every shape term at 0, "
                  "the plain ellipsoid. The line at height 0 is the coverslip: the nucleus sits on a basal layer of "
                  "cytoplasm.")


@register("struct-cytoplasm", "structures")
def struct_cytoplasm(ctx):
    step = 0.1
    out = {}
    for tag, opts in (("relaxed", {}), ("raw", {"p.cytoRelaxUm": 0})):
        cell, rg = cell_with_nucleus(ctx, tag, opts)[0]
        out[tag] = (cell, height_raster(cell, step), rg)
    cell, (img, x0, y0), rg = out["relaxed"]
    u = axis_of(cell["nucleus"])
    c = np.array([cell["nucleus"]["x"], cell["nucleus"]["y"]])
    o = np.asarray(cell["outline"])
    half = np.max(np.abs((o - c) @ u)) * 1.02
    p0, p1 = c - half * u, c + half * u
    hmax = float(max(img.max(), out["raw"][1][0].max()))

    def draw(fig, st):
        gs = fig.add_gridspec(1, 2, width_ratios=[1, 1.5])
        ax = fig.add_subplot(gs[0])
        ext = [x0, x0 + img.shape[1] * step, y0 + img.shape[0] * step, y0]
        im = ax.imshow(np.where(img > 0, img, np.nan), extent=ext, cmap="viridis", vmin=0, vmax=hmax)
        oo = np.vstack([o, o[:1]])
        ax.plot(oo[:, 0], oo[:, 1], color=st["fg"], lw=0.6)
        ax.plot([p0[0], p1[0]], [p0[1], p1[1]], color=COLORS[3], lw=1, ls="--")
        ax.set_aspect("equal")
        ax.set_xlabel("x (um)")
        ax.set_ylabel("y (um)")
        ax.set_title("relaxed height (default)")
        cb = fig.colorbar(im, ax=ax, shrink=0.75)
        cb.set_label("height (um)")
        ax2 = fig.add_subplot(gs[1])
        sx, sz = silhouette(rg, c, u)
        ax2.fill(sx, sz, color=COLORS[4], alpha=0.35, lw=0, label="nucleus (silhouette)")
        for tag, color, label in (("raw", COLORS[0], "raw profile (cytoRelaxUm 0)"),
                                  ("relaxed", COLORS[2], "relaxed, cytoRelaxUm 1 (default)")):
            im_t, xx0, yy0 = out[tag][1]
            s, h = sample_line(im_t, xx0, yy0, step, p0, p1)
            ax2.plot(s - half, h, color=color, lw=1.2, label=label)
        ax2.axhline(0, color=st["muted"], lw=0.8)
        ax2.set_xlabel("along the dashed line (um)")
        ax2.set_ylabel("height (um)")
        ax2.set_ylim(-0.1, hmax * 1.45)
        ax2.legend(loc="upper left", fontsize=6.5)
        ax2.set_title("section along the dashed line (height stretched)")

    body = ctx.plot("", draw, h=3.3, alt="Cytoplasm height map and a cross-section")
    return figure(body,
                  "The cytoplasm height of the cell nearest the spot, from the core's mesh (the BrightField volume, "
                  "microtubules and dyes use the same height). Right: a section along the nucleus's long axis. The "
                  "raw profile (a min/max of distance fields) has creases where its pieces meet; the relaxed one "
                  "solves h - l<sup>2</sup>&nabla;<sup>2</sup>h = h<sub>raw</sub> on a 0.25 um grid, with h = 0 on "
                  "the outline and at least the nucleus top plus its margin over the nucleus.")


def thin(p, step=0.2):
    """Every point about step um apart (for drawing: the centrelines are sampled much finer)."""
    if len(p) < 3:
        return p
    cum = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p[:, :2], axis=0), axis=1))]
    keep = np.unique(np.r_[np.searchsorted(cum, np.arange(0, cum[-1], step)), len(p) - 1])
    return p[keep]


def subset(mts, n=45):
    """Every k-th microtubule, about n of them (all of a cell's paths cover it completely)."""
    k = max(1, len(mts) // n)
    return mts[::k]


def mt_lines(cell):
    segs, zs = [], []
    for mt in subset(cell["mts"]):
        p = thin(np.asarray(mt, float))
        if len(p) > 1:
            segs.append(np.stack([p[:-1, :2], p[1:, :2]], axis=1))
            zs.append((p[:-1, 2] + p[1:, 2]) / 2)
    return np.concatenate(segs), np.concatenate(zs)


@register("struct-microtubules", "structures")
def struct_microtubules(ctx):
    cells = {tag: cell_with_nucleus(ctx, tag, opts)[0] for tag, opts in (("default", {}), ("kappa0", {"p.mtDirKappa": 0}))}
    zmax = max(max(p[2] for mt in c["mts"] for p in mt) for c, _ in cells.values())
    cell, rg = cells["default"]
    u = axis_of(cell["nucleus"])
    c0 = np.array([cell["nucleus"]["x"], cell["nucleus"]["y"]])

    def top(ax, cell, rg, st, title):
        o = np.vstack([cell["outline"], cell["outline"][:1]])
        ax.plot(o[:, 0], o[:, 1], color=st["muted"], lw=0.7)
        ring = widest(rg)
        ax.fill(ring[:, 0], ring[:, 1], color=COLORS[4], alpha=0.3, lw=0)
        segs, zs = mt_lines(cell)
        lc = LineCollection(segs, cmap="plasma", lw=0.9)
        lc.set_array(zs)
        lc.set_clim(0, zmax)
        ax.add_collection(lc)
        ax.set_aspect("equal")
        ax.autoscale_view()
        ax.set_ylim(ax.get_ylim()[::-1])
        ax.set_title(title, fontsize=8)
        ax.set_xlabel("x (um)")
        return lc

    def draw(fig, st):
        gs = fig.add_gridspec(2, 2, height_ratios=[1.7, 1])
        a0 = fig.add_subplot(gs[0, 0])
        lc = top(a0, cell, rg, st, "mtDirKappa 1.5 (default)")
        a0.set_ylabel("y (um)")
        c1, rg1 = cells["kappa0"]
        a1 = fig.add_subplot(gs[0, 1])
        top(a1, c1, rg1, st, "mtDirKappa 0: end point in any direction")
        cb = fig.colorbar(lc, ax=[a0, a1], shrink=0.8, location="right")
        cb.set_label("height (um)")
        a2 = fig.add_subplot(gs[1, :])
        side = [np.c_[(p[:, :2] - c0) @ u, p[:, 2]] for p in (thin(np.asarray(mt, float)) for mt in subset(cell["mts"]))
                if len(p) > 1]
        a2.add_collection(LineCollection(side, lw=0.7, color=COLORS[1], alpha=0.9))
        a2.autoscale_view()
        a2.fill(*silhouette(rg, c0, u), color=COLORS[4], alpha=0.35, lw=0)
        a2.axhline(0, color=st["muted"], lw=0.8)
        a2.set_xlabel("along the nucleus's long axis (um)")
        a2.set_ylabel("height (um)")
        a2.set_title("side view (default): the paths ride over or under the nucleus (pink)", fontsize=8)

    body = ctx.plot("", draw, h=5.4, alt="Microtubule paths of one cell")
    return figure(body,
                  "Microtubule centrelines of the cell nearest the spot, coloured by height: %d of its %d shown. Top: "
                  "with the default "
                  "direction weight the paths run outward from near the nucleus; with mtDirKappa 0 the end point "
                  "ignores direction and paths also cross over the nucleus. Bottom: the default paths seen from the "
                  "side, each a fraction of the local cytoplasm ceiling, over (or, starting low, under) the nucleus."
                  % (len(subset(cell["mts"])), len(cell["mts"])))


def local_frame(sites, piece):
    """Each site's arc length along the polyline, radial offset (um) and the local unit tangent."""
    a, b = piece[:-1], piece[1:]
    d = b - a
    L = np.linalg.norm(d, axis=1)
    t_hat = d / L[:, None]
    cum = np.r_[0, np.cumsum(L)]
    s_out, r_out, t_out = [], [], []
    for q in sites:
        tt = np.clip(np.einsum("ij,ij->i", q - a, t_hat), 0, L)
        foot = a + t_hat * tt[:, None]
        k = int(np.argmin(np.linalg.norm(q - foot, axis=1)))
        s_out.append(cum[k] + tt[k])
        r_out.append(q - foot[k])
        t_out.append(t_hat[k])
    return np.array(s_out), np.array(r_out).reshape(-1, 3), np.array(t_out).reshape(-1, 3)


def straight_piece(cell, length, clear=0.25):
    """The middle piece (length um) of the straightest, flattest microtubule with no other microtubule within clear
    um of it (so the sites near its axis are its own)."""
    mts = [np.asarray(mt, float) for mt in cell["mts"]]
    best = None
    for m, p in enumerate(mts):
        cum = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
        if cum[-1] < length + 2:
            continue
        i = int(np.searchsorted(cum, (cum[-1] - length) / 2))
        j = int(np.searchsorted(cum, cum[i] + length))
        piece = p[i:j + 1]
        lo, hi = piece.min(0) - clear, piece.max(0) + clear
        isolated = True
        for o, q in enumerate(mts):
            if o == m:
                continue
            q = q[np.all((q >= lo) & (q <= hi), axis=1)]
            if len(q) and np.min(np.linalg.norm(q[:, None, :] - piece[None, :, :], axis=2)) < clear:
                isolated = False
                break
        if not isolated:
            continue
        score = np.linalg.norm(piece[-1] - piece[0]) / (cum[j] - cum[i]) - abs(piece[-1, 2] - piece[0, 2])
        if best is None or score > best[0]:
            best = (score, piece)
    return best[1]


@register("struct-lattice", "structures")
def struct_lattice(ctx):
    cell, _ = cell_with_nucleus(ctx, "geo", {})[0]
    piece = straight_piece(cell, 1.2)
    length = float(np.linalg.norm(np.diff(piece, axis=0), axis=1).sum())
    lo, hi = piece.min(0) - 0.08, piece.max(0) + 0.08
    labelled = {}
    for pct in (100, 70, 25, 3):
        r = ctx.cli(dict(SPOT, **{"mt-label-pct": pct, "dyes-rect": [lo[0], lo[1], hi[0], hi[1]],
                                  "dyes-z": [lo[2], hi[2]], "dyes-t": [60, 60.05]}), tag="pct%d" % pct, dyes_json=True)
        sites = rows(load(r["dyes_json"])["sites"], 5)[:, :3]
        s, rv, th = local_frame(sites, piece)
        keep = (np.linalg.norm(rv, axis=1) < 0.04) & (s > 0.05) & (s < length - 0.05)
        labelled[pct] = (s[keep], rv[keep], th[keep])
    s, rv, th = labelled[100]
    e1 = np.cross(th, [0, 0, 1.0])
    e1 /= np.linalg.norm(e1, axis=1)[:, None]
    e2 = np.cross(th, e1)
    x1, x2 = np.einsum("ij,ij->i", rv, e1) * 1000, np.einsum("ij,ij->i", rv, e2) * 1000
    radius = np.hypot(x1, x2)
    span = s.max() - s.min()
    per_um = len(s) / span

    def draw(fig, st):
        gs = fig.add_gridspec(2, 2, width_ratios=[1, 2.2], height_ratios=[1, 1])
        a = fig.add_subplot(gs[:, 0])
        a.scatter(x1, x2, s=1.2, color=COLORS[1], lw=0)
        t = np.linspace(0, 2 * np.pi, 200)
        for R in (12.5, 24.5):
            a.plot(R * np.cos(t), R * np.sin(t), color=st["muted"], lw=0.6, ls="--")
        a.set_aspect("equal")
        a.set_xlim(-36, 36)
        a.set_ylim(-36, 36)
        a.set_xlabel("nm")
        a.set_ylabel("nm")
        a.set_title("end-on, every site labelled\n(%d dyes over %.2f um)" % (len(s), span), fontsize=7.5)
        b = fig.add_subplot(gs[0, 1])
        b.hist(np.degrees(np.arctan2(x2, x1)), bins=np.linspace(-180, 180, 145), color=COLORS[1])
        b.set_xlabel("angle around the axis (deg)")
        b.set_ylabel("dyes")
        b.set_xticks([-180, -90, 0, 90, 180])
        b.set_title("13 protofilaments: %.0f dyes per um (13 per 8 nm dimer: 1625)" % per_um, fontsize=7.5)
        c = fig.add_subplot(gs[1, 1])
        for k, pct in enumerate((70, 25, 3)):
            ss = labelled[pct][0]
            c.plot(ss - s.min(), np.full(len(ss), -k), "|", ms=7, color=COLORS[k], mew=0.5)
        c.set_yticks([0, -1, -2], ["70 % (DNA-PAINT)", "25 % (PALM)", "3 % (dSTORM)"])
        c.set_xlabel("along the axis (um)")
        c.set_ylim(-2.6, 0.6)
        c.set_xlim(0, span)
        c.set_title("the labelled sites at each mode's typical share", fontsize=7.5)

    body = ctx.plot("", draw, h=3.6, alt="Dye sites on one microtubule")
    return figure(body,
                  "Dye positions on a %.1f um piece of one microtubule, from the core's site query (each site's "
                  "offset from the centreline). End-on, the dyes form a ring: the 12.5 nm lattice (inner dashed "
                  "circle) plus the 12 nm binder (outer), spread by the 2-5 nm linker; measured radius %s &plusmn; "
                  "%s nm. Around the axis: 13 peaks, one per protofilament, widened by the linker. Bottom: the sites "
                  "that carry a dye at "
                  "each mode's typical labelling." % (length, sig(radius.mean(), 3), sig(radius.std(), 2)))
