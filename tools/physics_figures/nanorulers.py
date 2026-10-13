"""Nanorulers page (docs/samples/nanorulers/): the bent rods and their docking strands from above, the bending
statistics against the 2D worm-like chain, and the rods in a field of view."""
import numpy as np

from .common import COLORS, figure, load, register, sig

L_NM = 200.0                     # the Nanoruler body length at the default 80 nm spacing (data/origami/designs.json), for the theory lines
SPOT_GAP_NM = 160.0              # its outer spots (A at 20 nm, C at 180 nm)
CENTRE = {"x": 0.0, "y": 0.0}


def geometry(ctx, size_um, tag, detail=True, **opts):
    r = ctx.cli(dict(CENTRE, specimen="Nanorulers", **{"geometry-um": size_um, "geometry-detail": 1 if detail else 0},
                     **opts), tag=tag, geometry_json=True)
    g = load(r["geometry_json"])
    return g


def spot_centres(m):
    """The bent anchor positions (molecule frame, nm) of each spot's designed strands, averaged per spot."""
    s = np.asarray(m["sites"], dtype=np.float64)
    return {int(k): s[s[:, 0] == k][:, 8:10].mean(axis=0) for k in np.unique(s[:, 0])}


def draw_rods(ax, geo, st, dye_size=6.0):
    for m in geo["origami"]:
        o = np.asarray(m["outline"] + m["outline"][:1])
        ax.fill(o[:, 0], o[:, 1], color=COLORS[1], alpha=0.35, lw=0)
        ax.plot(o[:, 0], o[:, 1], color=COLORS[1], lw=0.6)
        s = np.asarray(m.get("sites", []), dtype=np.float64)
        for k in range(3):
            on = s[(s[:, 0] == k) & (s[:, 10] > 0)] if len(s) else s
            off = s[(s[:, 0] == k) & (s[:, 10] == 0)] if len(s) else s
            ax.plot(on[:, 2], on[:, 3], "o", ms=dye_size ** 0.5, color=COLORS[[0, 2, 3][k]], lw=0)
            ax.plot(off[:, 2], off[:, 3], "o", ms=dye_size ** 0.5, mfc="none", mew=0.4, color=st["muted"])


def molecule_frame(m, xy_um):
    """World points (um) in the molecule's frame (nm): the chord along +x, its midpoint at the origin."""
    p = (np.asarray(xy_um, dtype=np.float64) - [m["x"], m["y"]]) * 1000.0
    c, s = np.cos(m["yaw"]), np.sin(m["yaw"])
    return np.stack([c * p[:, 0] + s * p[:, 1], -s * p[:, 0] + c * p[:, 1]], axis=1)


@register("ruler-shapes", "nanorulers")
def ruler_shapes(ctx):
    size, n_rods, gap = 6.0, 8, 60.0
    cases = [("rigid", {"origami-bend": 0}, "Rigid (Bending Off)"),
             ("lp1500", {}, "Lp 1500 nm (default)")]
    geos = [(geometry(ctx, size, tag, **o)["origami"][:n_rods], title) for tag, o, title in cases]

    def draw(fig, st):
        axs = fig.subplots(1, 2, sharey=True)
        for ax, (mols, title) in zip(axs, geos):
            for j, m in enumerate(mols):
                dy = j * gap
                o = molecule_frame(m, m["outline"] + m["outline"][:1])
                ax.fill(o[:, 0], o[:, 1] + dy, color=COLORS[1], alpha=0.35, lw=0)
                ax.plot(o[:, 0], o[:, 1] + dy, color=COLORS[1], lw=0.6)
                s = np.asarray(m["sites"], dtype=np.float64)
                d = molecule_frame(m, s[:, 2:4])
                for k in range(3):
                    on, off = (s[:, 0] == k) & (s[:, 10] > 0), (s[:, 0] == k) & (s[:, 10] == 0)
                    ax.plot(d[on, 0], d[on, 1] + dy, "o", ms=2.6, color=COLORS[[0, 2, 3][k]], lw=0)
                    ax.plot(d[off, 0], d[off, 1] + dy, "o", ms=2.6, mfc="none", mew=0.5, color=st["muted"])
            ax.set_xlim(-115, 115)
            ax.set_ylim((len(mols) - 0.4) * gap, -0.6 * gap)
            ax.set_aspect("equal")
            ax.set_title(title)
            ax.set_xlabel("Along the rod (nm)")
            ax.set_yticks([])
        axs[0].set_ylabel("%d rods, stacked %g nm apart" % (n_rods, gap))

    body = ctx.plot("", draw, w=4.6, h=3.9, alt="Single nanorulers: rigid and at Lp 1500 nm")
    return figure(body,
                  "The first %d rods of a nanoruler field, each in its own frame (the line between its ends along x, "
                  "its middle at 0) and stacked %g nm apart: the bent 13 nm wide body (blue) and the dyes of its "
                  "docking strands, coloured by spot (A, B, C at 20, 100 and 180 nm along it; hollow grey: a strand "
                  "the labelling left without a dye, 80 %% incorporation). The same rods, landing and labels in both: "
                  "only the bending changes. Left: rigid; right: the default persistence length of 1500 nm. Each "
                  "spot's five strands are a ring around the rod seen from above (a random roll per rod), so they "
                  "spread across it; the dyes sit up to 3 nm (the linker) off their anchors." % (n_rods, gap))


@register("ruler-wlc", "nanorulers")
def ruler_wlc(ctx):
    size = 40.0
    res = []
    for tag, lp in (("lp1500", 1500.0),):
        geo = geometry(ctx, size, tag)
        ends = np.degrees([m["endAngle"] for m in geo["origami"]])
        dist = []
        for m in geo["origami"]:
            c = spot_centres(m)
            dist.append(float(np.hypot(*(c[2] - c[0]))))
        folds = sum(1 for m in geo["origami"] if m["fold"])
        res.append((lp, ends, np.asarray(dist), folds))

    def r_theory(lp, s):
        """RMS end-to-end distance of a 2D worm-like chain of contour s (tangent correlation exp(-s / 2 Lp))."""
        return np.sqrt(4 * lp * s * (1 - 2 * lp / s * (1 - np.exp(-s / (2 * lp)))))

    def draw(fig, st):
        axs = fig.subplots(1, 2)
        lim = 4 * np.degrees(np.sqrt(L_NM / 1500.0))
        xs = np.linspace(-lim, lim, 400)
        bins = np.linspace(-lim, lim, 41)
        for k, (lp, ends, dist, folds) in enumerate(res):
            sd = np.degrees(np.sqrt(L_NM / lp))
            axs[0].hist(ends, bins=bins, density=True, color=COLORS[k], alpha=0.45,
                        label="Lp %g nm: RMS %s° (%d rods)" % (lp, sig(np.sqrt(np.mean(ends ** 2)), 3), len(ends)))
            axs[0].plot(xs, np.exp(-xs ** 2 / (2 * sd ** 2)) / (sd * np.sqrt(2 * np.pi)), "--", color=COLORS[k],
                        lw=1, label="theory: %s°" % sig(sd, 3))
        axs[0].set_xlabel("End angle θ(L) − θ(0) (°)")
        axs[0].set_ylabel("Density")
        axs[0].legend(fontsize=6.5, loc="upper left")
        lo = min(d.min() for _, _, d, _ in res)
        dbins = np.linspace(np.floor(lo), SPOT_GAP_NM + 0.5, 40)
        for k, (lp, ends, dist, folds) in enumerate(res):
            axs[1].hist(dist, bins=dbins, density=True, color=COLORS[k], alpha=0.45,
                        label="Lp %g nm: RMS %s nm" % (lp, sig(np.sqrt(np.mean(dist ** 2)), 4)))
            axs[1].axvline(r_theory(lp, SPOT_GAP_NM), color=COLORS[k], ls="--", lw=1,
                           label="theory %s nm" % sig(r_theory(lp, SPOT_GAP_NM), 4))
        axs[1].axvline(SPOT_GAP_NM, color=st["muted"], lw=1, label="straight: %g nm" % SPOT_GAP_NM)
        axs[1].set_xlabel("Distance of spots A and C (nm)")
        axs[1].legend(fontsize=6.5, loc="upper left")

    body = ctx.plot("", draw, h=2.7, alt="End angles and spot distances against the 2D worm-like chain")
    return figure(body,
                  "Bending statistics of the rods in a %g µm square, at the default persistence length of 1500 nm. Left: the angle between the two end tangents, a normal distribution of variance L/Lp "
                  "(L = 200 nm) for a 2D worm-like chain. Right: the distance between the centres of the outer spots "
                  "(160 nm apart along the rod); bending only shortens it. The dashed lines are the 2D chain's RMS "
                  "end-to-end distance over 160 nm, √(4 Lp s [1 − (2 Lp/s)(1 − e<sup>−s/2Lp</sup>)]) "
                  "[[rivetti1996](../references.md#rivetti1996)]. Rods whose bend folds a cross-section over "
                  "(curvature × 6.5 nm ≥ 1): %d of them." % (size, res[0][3]))


@register("ruler-field", "nanorulers")
def ruler_field(ctx):
    px = 64
    geo = geometry(ctx, px * 0.1, "fov", detail=True)

    def draw(fig, st):
        ax = fig.subplots()
        draw_rods(ax, geo, st, dye_size=4.0)
        ax.set_xlim(geo["x0"], geo["x1"])
        ax.set_ylim(geo["y1"], geo["y0"])
        ax.set_aspect("equal")
        ax.set_xlabel("x (µm)")
        ax.set_ylabel("y (µm)")

    body = ctx.plot("", draw, w=4.6, h=4.6, alt="The nanorulers in a field of view")
    return figure(body,
                  "The nanorulers in a %d × %d px field of view of 100 nm pixels (%d rods at the default 1.2 per µm²): "
                  "the bent bodies and their dyes, coloured by spot as above. The spots of a rod lie 80 nm apart, less "
                  "than a pixel: the camera sees each rod as one blur, and only localising the single binding events "
                  "(the viewer's Localisations layer, or any SMLM software) separates them." %
                  (px, px, len(geo["origami"])))


@register("ruler-spacing", "nanorulers")
def ruler_spacing(ctx):
    spacings = [160, 80, 40, 20]
    rods = [(s, geometry(ctx, 4.0, "rod%d" % s, **{"origami-spacing-nm": s, "origami-bend": 0})["origami"][0])
            for s in spacings]
    logo = geometry(ctx, 4.0, "logo20", **{"origami-design": "TUDelftLogo", "origami-spacing-nm": 20})["origami"][0]

    def draw(fig, st):
        axs = fig.subplots(1, 2, gridspec_kw={"width_ratios": [1.1, 1]})
        ax = axs[0]
        for j, (s, m) in enumerate(rods):
            dy = j * 40.0
            o = molecule_frame(m, m["outline"] + m["outline"][:1])
            ax.fill(o[:, 0], o[:, 1] + dy, color=COLORS[1], alpha=0.35, lw=0)
            ax.plot(o[:, 0], o[:, 1] + dy, color=COLORS[1], lw=0.6)
            d = molecule_frame(m, np.asarray(m["sites"], dtype=np.float64)[:, 2:4])
            k = np.asarray(m["sites"], dtype=np.float64)[:, 0].astype(int)
            for q in range(3):
                ax.plot(d[k == q, 0], d[k == q, 1] + dy, "o", ms=2.2, color=COLORS[[0, 2, 3][q]], lw=0)
            ax.text(o[:, 0].max() + 8, dy, "%d nm" % s, va="center", fontsize=8, color=st["muted"])
        ax.set_xlim(-200, 240)
        ax.set_ylim(len(rods) * 40 - 15, -25)
        ax.set_aspect("equal")
        ax.set_yticks([])
        ax.set_xlabel("Along the rod (nm)")
        ax.set_title("Nanoruler at four spacings")
        ax = axs[1]
        o = molecule_frame(logo, logo["outline"] + logo["outline"][:1])
        ax.fill(o[:, 0], o[:, 1], color=COLORS[1], alpha=0.2, lw=0)
        ax.plot(o[:, 0], o[:, 1], color=COLORS[1], lw=0.6)
        s = np.asarray(logo["sites"], dtype=np.float64)
        d = molecule_frame(logo, s[:, 2:4])
        on = s[:, 10] > 0
        ax.plot(d[on, 0], d[on, 1], "o", ms=3.2, color=COLORS[0], lw=0)
        ax.plot(d[~on, 0], d[~on, 1], "o", ms=3.2, mfc="none", mew=0.5, color=st["muted"])
        ax.set_xlim(o[:, 0].min() - 10, o[:, 0].max() + 10)
        ax.set_ylim(o[:, 1].max() + 10, o[:, 1].min() - 10)
        ax.set_aspect("equal")
        ax.set_xlabel("x (nm)")
        ax.set_ylabel("y (nm)")
        ax.set_title("TU Delft logo, 20 nm")

    body = ctx.plot("", draw, w=4.6, h=2.6, alt="Nanorulers at several site spacings and the TU Delft logo 2D origami")
    return figure(body,
                  "The site spacing (SpacingNm). Left: a rigid nanoruler at 160, 80, 40 and 20 nm spacing, each in its "
                  "own frame: the spots (A, B, C) stay one spacing apart and the rod keeps 20 nm beyond the outer "
                  "ones, so it is 2 x spacing + 40 nm long. Right: the TU Delft logo 2D origami at 20 nm, a rigid "
                  "plate covering its 17 x 11 hex map, one docking strand on each of the 37 logo hexagons (hollow "
                  "grey: a strand left without a dye, 80 % incorporation), lying face up.")
