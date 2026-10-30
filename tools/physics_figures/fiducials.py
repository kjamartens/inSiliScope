"""Fiducial beads page (docs/samples/fiducials.md): each bead type's spectra against a light path and what it detects
(the cli's --setup-json `fiducials`), beads at the three heights in one cell-field frame, and clumps on the bead sample."""
import numpy as np

from .common import COLORS, SPOT, figure, img, load, robust_range, register, sig, stack, table

TYPES = ["Gold", "TetraSpeck", "FluoSpheresOrange", "FluoSpheresRed", "FluoSpheresDarkRed", "FluoSpheresInfrared",
         "Nanodiamond"]
# The light path of the figure: the default cell field's two DNA-PAINT labels (pores Cy3B, microtubules ATTO 655), the
# auto multi-band set at 561 + 640 nm, plus the 488 nm line.
LIGHT = {"npc-dye": "Cy3B", "laser-488": 1}


def wavelengths(lp):
    lo, step, n = lp["grid_nm"]
    return lo + step * np.arange(int(n))


def duration(sec):
    """A half-time in s, min or h, two significant digits."""
    return "%s s" % sig(sec, 2) if sec < 120 else "%s min" % sig(sec / 60, 2) if sec < 7200 else "%s h" % sig(sec / 3600, 2)


@register("fid-spectra", "fiducials")
def fid_spectra(ctx):
    setups = [load(ctx.cli(dict(LIGHT, size=32, frames=1, **{"fid-type": t, "prepare": 1}), tag=t,
                           setup_json=True)["setup_json"]) for t in TYPES]
    lp = setups[0]["light_path"]
    wl = wavelengths(lp)
    lines = [l for l in lp["lasers"] if l["at_sample_kw_per_cm2"] > 0]
    passT = np.asarray(lp["emission_T"]) * np.asarray(lp["dichroic_T"])

    def draw(fig, st):
        axs = fig.subplots(2, 4, sharex=True, sharey=True).ravel()
        for ax, s in zip(axs, setups):
            f = s["fiducials"]
            ax.fill_between(wl, 0, passT, color=st["grid"], alpha=0.6, lw=0)
            for l in lines:
                ax.axvline(l["nm"], color=st["muted"], lw=0.8, ls=":")
            k = 0
            for c in f["components"]:
                if c["scatter"] or "em" not in c:
                    continue
                col = COLORS[[5, 2, 0, 3, 4, 1, 6][k % 7]]
                ex, em = np.asarray(c["ex"]), np.asarray(c["em"])
                ax.plot(wl, ex / max(ex.max(), 1e-30), color=col, lw=0.9, ls="--")
                ax.plot(wl, em / max(em.max(), 1e-30), color=col, lw=1.2)
                k += 1
            ax.set_title(f["name"].split(" (")[0], fontsize=8)
            ax.set_xlim(350, 850)
            ax.set_ylim(0, 1.05)
            ax.tick_params(labelsize=7)
        ax = axs[-1]
        ax.axis("off")
        ax.plot([], [], color=st["fg"], lw=0.9, ls="--", label="excitation")
        ax.plot([], [], color=st["fg"], lw=1.2, label="emission")
        ax.fill_between([], [], color=st["grid"], alpha=0.6, label="dichroic × emission filter")
        ax.plot([], [], color=st["muted"], lw=0.8, ls=":", label="laser lines")
        ax.legend(loc="center", fontsize=7, frameon=False)
        for a in axs[4:7]:
            a.set_xlabel("Wavelength (nm)", fontsize=8)
        axs[0].set_ylabel("Normalised", fontsize=8)
        axs[4].set_ylabel("Normalised", fontsize=8)

    body = ctx.plot("", draw, w=7.6, h=3.8, alt="The bead types' spectra against the light path")
    rows = []
    for s in setups:
        f = s["fiducials"]
        for c in f["components"]:
            lit = c["photons_per_s"] > 0
            half = "–" if not lit else "never" if c["half_s"] < 0 else duration(c["half_s"])
            rows.append([f["name"] if c is f["components"][0] else "", c["name"], "%.0f" % c["lambda_nm"],
                         "–" if c["scatter"] else sig(c["emitters"], 3), sig(c["photons_per_s"], 2) if lit else "0",
                         "–" if c["scatter"] else "%.2g %%" % (100 * c["detected_fraction"]), half])
    names = ", ".join("%g nm" % l["nm"] for l in lines)
    return figure(body,
                  "The bead types (data/fiducials: Balinovic et al. 2019, FPbase): each dye's excitation (dashed) and "
                  "emission (solid), against the light path of the default cell field's labels (%s at 1 kW/cm², %s, "
                  "%s; grey: what the dichroic and the emission filter pass). The table gives, per component, the "
                  "wavelength its PSF is made at, its equivalent dyes (ε 10⁵, QY 0.5) at the product size, the photons "
                  "per second the camera detects from one bead of that size at the beam's peak, the share of its "
                  "emission detected, and its fluorescence half-time under this light (the 288 s reference is at "
                  "1 kW/cm² at the dye's own excitation peak; off the peak a bead bleaches more slowly; – : no line "
                  "excites it). Gold's scattered laser light passes the filters only through their blocking floor "
                  "(OD 8)." %
                  (names, lp["dichroic"], lp["emission_filter"])) + \
        table(["Type", "Component", "PSF λ (nm)", "Equiv. dyes", "Detected (photons/s)", "Detected share", "Half-time"],
              rows)


@register("fid-heights", "fiducials")
def fid_heights(ctx):
    base = dict(SPOT, size=256, frames=1, **{"mt-dye": "None", "npc-dye": "None", "laser-561": 1, "laser-640": 0,
                                              "fid-type": "TetraSpeck", "fid-dyes": 2, "fid-density": 0.05})
    cases = [("On the coverslip", 0), ("On top of the sample", 1), ("In a 10 µm gel layer", 2)]
    tiles = []
    for name, h in cases:
        r = ctx.cli(dict(base, **{"fid-height": h}), tag="h%d" % h, out=True)
        a = stack(r["out"])[0].astype(np.float64)
        lo, hi = robust_range(a, 1, 99.95)
        tiles.append(ctx.tile("h%d" % h, a, cmap="gray", vmin=lo, vmax=max(hi, lo + 1)))
    return table([c[0] for c in cases], [[img(t) for t in tiles]], label_col=False) + \
        "\nThe same 25.6 µm field of the cell field (256 px of 100 nm, one 50 ms frame, focus 0.5 µm above the " \
        "coverslip, no labels: only the beads emit) with TetraSpeck beads at 0.05 per µm² (made dim here, 2 " \
        "equivalent dyes, so none saturates), at the three heights: on the glass (sharp at this focus, also under " \
        "the cells), on the sample's highest surface (the cells' tops several µm above the focus: wide rings, the " \
        "bare glass between them sharp) and anywhere in a 10 µm gel outside the cells (most far out of focus).\n"


@register("fid-clumps", "fiducials")
def fid_clumps(ctx):
    base = dict(SPOT, size=256, frames=1, specimen="Beads")   # its defaults: gold, 0.3 per um^2, 561 nm at 0.1 kW/cm^2
    cases = [("Aggregation 0 %", 0), ("30 %", 30), ("70 %", 70)]
    arrays = []
    for _, p in cases:
        r = ctx.cli(dict(base, **{"fid-aggregation-pct": p}), tag="a%d" % p, out=True)
        arrays.append(stack(r["out"])[0].astype(np.float64))
    lo, hi = robust_range(np.concatenate([a.ravel() for a in arrays]), 1, 99.95)
    tiles = [ctx.tile("a%d" % p, a, cmap="gray", vmin=lo, vmax=max(hi, lo + 1)) for (_, p), a in zip(cases, arrays)]
    return table([c[0] for c in cases], [[img(t) for t in tiles]], label_col=False) + \
        "\nThe bead sample at its defaults (a bare coverslip, 100 nm gold at 0.3 beads per µm², 561 nm at 0.1 kW/cm², " \
        "focus on the beads' centres) but the aggregation, one 50 ms frame of 256 px of 100 nm, one contrast for all " \
        "three. The density counts beads, so the more stick together, the fewer and brighter the spots: at 30 % " \
        "half the beads are in clumps (mean 1.4 beads), at 70 % nearly all (mean 3.3); a clump is far smaller than " \
        "the PSF, so it looks like one brighter bead.\n"
