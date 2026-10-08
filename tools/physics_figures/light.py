"""Dyes and light path page: spectra through the light path, the four label modes, the shutters, the DNA-PAINT
imager background, and mean field vs per-dye rendering of continuous populations."""
import numpy as np
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

from .common import (BUILD_INFO, COLORS, SPOT, figure, img, load, register, robust_range, secs, sig, stack, table)

MODES = ["dSTORM", "PALM", "DNA-PAINT", "WideField"]


def typical(mode):
    """The microtubules' typical label in a mode (data/dyes/library.json typicalLabels) with the mode's light preset."""
    return {"mt-mode": mode, "mt-dye": -1, "light-preset": "auto"}


def wavelengths(setup):
    lo, step, n = setup["light_path"]["grid_nm"]
    return lo + step * np.arange(int(n))


def label_name(lab):
    return "%s %s" % (lab["dye_name"], lab["mode"])


def gray(ctx, name, a):
    lo, hi = robust_range(a, 0.5, 99.9)
    return ctx.tile(name, a, cmap="gray", vmin=lo, vmax=max(hi, lo + 1))


@register("light-spectra", "dyes-and-light-path")
def light_spectra(ctx):
    setups = [load(ctx.cli(typical(m), tag=m, setup_json=True)["setup_json"]) for m in MODES]

    def draw(fig, st):
        axs = fig.subplots(2, 2, sharex=True).ravel()
        for ax, s in zip(axs, setups):
            lab, lp = s["labels"][0], s["light_path"]
            wl = wavelengths(s)
            ax.plot(wl, lp["dichroic_T"], color=st["muted"], lw=0.8)
            ax.fill_between(wl, 0, lp["emission_T"], color=st["muted"], alpha=0.15, lw=0)
            ax.plot(wl, lp["qe"], color=st["muted"], lw=0.8, ls=":")
            states = [("main", lab["main"])] + ([("pre", lab["pre"])] if lab["pre"].get("emits") else [])
            for name, state in states:
                c = COLORS[3] if name == "main" else COLORS[2]
                ax.plot(wl, state["ex"], color=c, lw=0.9, ls="--")
                ax.plot(wl, state["em"], color=c, lw=1.0)
                ax.fill_between(wl, 0, state["detected"], color=c, alpha=0.45, lw=0)
            for L in lp["lasers"]:
                if L["at_sample_kw_per_cm2"] > 0:
                    ax.axvline(L["nm"], color=COLORS[5], lw=1.4)
                    ax.text(L["nm"], 1.03, "%g" % L["nm"], color=COLORS[5], fontsize=6.5, ha="center", va="bottom")
            m = lab["main"]
            txt = "F = %.2f at %.0f nm, %s photons/s while on" % (m["detected_fraction"], m["lambda_nm"],
                                                                 sig(m["detected_per_s"], 2, tex=True))
            if lab["pre"].get("emits"):
                txt += "\ngreen pre state: F = %.2f" % lab["pre"]["detected_fraction"]
            ax.set_title("%s\n%s" % (label_name(lab), txt), fontsize=7.5)
            ax.set_xlim(380, 820)
            ax.set_ylim(0, 1.12)
        for ax in axs[2:]:
            ax.set_xlabel("wavelength (nm)")
        h = [Line2D([], [], color=st["fg"], ls="--", lw=0.9), Line2D([], [], color=st["fg"], lw=1.0),
             Patch(color=st["fg"], alpha=0.45, lw=0), Line2D([], [], color=COLORS[5], lw=1.4),
             Line2D([], [], color=st["muted"], lw=0.8), Patch(color=st["muted"], alpha=0.25, lw=0),
             Line2D([], [], color=st["muted"], lw=0.8, ls=":")]
        l = ["excitation", "emission", "detected", "laser", "dichroic T", "emission filter T", "camera QE"]
        fig.legend(h, l, loc="outside lower center", ncol=7, fontsize=6.5)

    body = ctx.plot("", draw, h=4.9, alt="Dye spectra through the light path")
    return figure(body,
                  "The typical microtubule label of each mode with its light preset, as a movie resolves it (the "
                  "cli's setup output). The detected part of the emission passes the dichroic, the emission filter "
                  "and the camera's QE; F is its fraction. mEos3.2's green pre state (before photoconversion) is "
                  "mostly blocked by the PALM filter. Photons per second: a dye while it emits, after the "
                  "objective's collection efficiency.")


def movie(ctx, opts, tag, frames=50, size=96):
    r = ctx.cli(dict(SPOT, size=size, frames=frames, **opts), tag=tag, out=True, photons_out=True, setup_json=True)
    return stack(r["out"]), stack(r["photons_out"]), load(r["setup_json"]), r


@register("light-modes", "dyes-and-light-path")
def light_modes(ctx):
    cols, frames, means = [], [], []
    for m in MODES:
        adu, ph, setup, _ = movie(ctx, typical(m), m, frames=20 if m == "WideField" else 50)
        lab = setup["labels"][0]
        frames.append(gray(ctx, m + "-frame", adu[0]))
        mean = ph.mean(0)
        means.append(ctx.tile(m + "-mean", mean, cmap="magma", vmin=0, vmax=robust_range(mean, hi=99.9)[1]))
        cols.append("%s<br>%s, %g %%" % (m, lab["dye_name"], lab["labelled_pct"]))
    return table([""] + cols,
                 [["one frame (ADU)"] + [img(p) for p in frames],
                  ["mean photons"] + [img(p) for p in means]]) + \
        "\nThe same 9.6 um field (96 px of 100 nm, 50 ms frames from t = 60 s) with each mode's typical label, " \
        "labelled share and light preset. Top: one camera frame. Bottom: the mean of the frames' photon images " \
        "before the camera (50 frames, WideField 20; noiseless). The blinking modes show a few emitters per frame " \
        "and the structure only over many frames; WideField shows it in every frame.\n"


@register("light-shutters", "dyes-and-light-path")
def light_shutters(ctx):
    cases = [("lasers", 1, 0), ("lamp", 0, 1), ("both", 1, 1), ("none", 0, 0)]
    tiles, stats = [], []
    for name, epi, trans in cases:
        r = ctx.cli(dict(SPOT, size=96, frames=1, **{"light-epi": epi, "light-trans": trans,
                                                    "bf-photons-per-px-per-sec": 5000}), tag=name, out=True)
        a = stack(r["out"])[0]
        tiles.append(gray(ctx, name, a))
        stats.append("%.0f / %.0f / %.0f" % (a.min(), np.median(a), a.max()))
    return table(["", "lasers (epi)", "lamp (transmitted)", "both", "none"],
                 [["one frame"] + [img(t) for t in tiles], ["ADU min / median / max"] + stats]) + \
        "\nThe two shutters (cli `light-epi`, `light-trans`; Micro-Manager's `Lasers` and `TransmittedLamp`) with " \
        "the default label (ATTO 655 DNA-PAINT) and the lamp turned down to 5000 photons/px/s so both lights show. " \
        "Both open: the fluorescence photons plus the lamp's, through one noise chain. None: offset, read noise " \
        "and dark current only. Each image has its own contrast.\n"


@register("light-imager", "dyes-and-light-path")
def light_imager(ctx):
    concs = [0.3, 1, 3, 10]
    tiles, model, measured = [], [], []
    for c in concs:
        adu, ph, setup, _ = movie(ctx, {"mt-imager-nm": c}, "c%g" % c, frames=5)
        lab = setup["labels"][0]
        model.append(lab["imager_background_per_px_per_s"] * setup["frame"]["exposure_s"])
        measured.append(float(np.median(ph)))
        tiles.append(gray(ctx, "c%g" % c, adu[0]))
    return table(["imager"] + ["%g nM" % c for c in concs],
                 [["one frame (ADU)"] + [img(t) for t in tiles],
                  ["free-imager background, model (photons/px/frame)"] + [sig(v, 3) for v in model],
                  ["median photons/px/frame, rendered"] + [sig(v, 3) for v in measured]]) + \
        "\nThe DNA-PAINT imager concentration sets the binding rate (k<sub>on</sub>c: more blinks) and the free " \
        "imager's flat background c N<sub>A</sub> H A<sub>px</sub> k<sub>em</sub>&eta;F (model row, from the cli's " \
        "setup output). The rendered row is the median of the photon images before the camera, which also holds " \
        "the blinks' halos. Default 1 nM; frames 50 ms.\n"


@register("light-meanfield", "dyes-and-light-path", timed=True)
def light_meanfield(ctx):
    base = dict(SPOT, size=64, frames=1, **typical("WideField"), **{"mt-label-pct": 10})
    runs = {}
    for name, opts in (("mean field", {"mean-field-density-per-um2": 0}),
                       ("per dye", {"mean-field-density-per-um2": 1e12, "mean-field-max-emitters": 1e12})):
        r = ctx.cli(dict(base, **opts), tag=name.replace(" ", ""), photons_out=True, fresh=True)
        runs[name] = (stack(r["photons_out"])[0], r.time("photons_out"))
    mf, pd = runs["mean field"][0], runs["per dye"][0]
    d = mf - pd
    scale = float(np.abs(d).max())
    vmax = robust_range(pd, hi=99.9)[1]
    t_mf, t_pd = runs["mean field"][1], runs["per dye"][1]
    return table(["", "per dye", "mean field", "mean field - per dye"],
                 [["photons", img(ctx.tile("perdye", pd, vmin=0, vmax=vmax)),
                   img(ctx.tile("meanfield", mf, vmin=0, vmax=vmax)), img(ctx.diff_tile("diff", d, scale))],
                  ["how", "each dye's window splatted", "dye density on z planes x PSF",
                   "&plusmn;%s photons = %s %% of the mean" % (sig(scale, 2), sig(100 * scale / pd.mean(), 2))],
                  ["total photons", sig(pd.sum(), 4), sig(mf.sum(), 4), "%+.2f %%" % (100 * (mf.sum() / pd.sum() - 1))],
                  ["rms difference per pixel", "", "", "%s %% of the mean" % sig(100 * np.sqrt(np.mean(d ** 2)) / pd.mean(), 2)],
                  ["setup + frame", "%s + %s" % (secs(t_pd[0]), secs(t_pd[1] - t_pd[0])),
                   "%s + %s" % (secs(t_mf[0]), secs(t_mf[1] - t_mf[0])), ""]]) + \
        "\nA continuous population (mEGFP WideField, 10 %% of the sites labelled, one 50 ms frame of 64 px) " \
        "rendered both ways, forced by the switch's thresholds; by default the renderer picks mean field above " \
        "20 emitting dyes per um<sup>2</sup> of the 500 nm focal slab (`mean-field-density-per-um2`). Times on " \
        "the build machine (%d threads).\n" % BUILD_INFO.get("cores", 0)
