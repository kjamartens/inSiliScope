"""Dyes and light path page: spectra through the light path, the four label modes, the shutters, the DNA-PAINT
imager background, and mean field vs per-dye rendering of continuous populations."""
import numpy as np
from matplotlib.legend_handler import HandlerTuple
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
    MAIN, PRE, LASER = COLORS[3], COLORS[2], COLORS[5]

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
                c = MAIN if name == "main" else PRE
                ax.plot(wl, state["ex"], color=c, lw=0.9, ls="--")
                ax.plot(wl, state["em"], color=c, lw=1.0)
                ax.fill_between(wl, 0, state["detected"], color=c, alpha=0.45, lw=0)
            for L in lp["lasers"]:
                if L["at_sample_kw_per_cm2"] > 0:
                    ax.axvline(L["nm"], color=LASER, lw=1.4)
                    ax.text(L["nm"] + 5, 1.06, "%g" % L["nm"], color=LASER, fontsize=6.5, ha="left", va="center")
            m = lab["main"]
            txt = "F = %.2f at %.0f nm, %s photons/s while on" % (m["detected_fraction"], m["lambda_nm"],
                                                                 sig(m["detected_per_s"], 2, tex=True))
            if lab["pre"].get("emits"):
                txt += "\nGreen pre state: F = %.2f" % lab["pre"]["detected_fraction"]
            ax.set_title("%s\n%s" % (label_name(lab), txt), fontsize=7.5)
            ax.set_xlim(380, 820)
            ax.set_ylim(0, 1.12)
        for ax in axs[2:]:
            ax.set_xlabel("Wavelength (nm)")
        for ax in axs[::2]:
            ax.set_ylabel("Relative / transmission")
        h = [Line2D([], [], color=MAIN, ls="--", lw=0.9), Line2D([], [], color=MAIN, lw=1.0),
             Patch(color=MAIN, alpha=0.45, lw=0),
             (Line2D([], [], color=PRE, ls="--", lw=0.9), Line2D([], [], color=PRE, lw=1.0)),
             Line2D([], [], color=LASER, lw=1.4), Line2D([], [], color=st["muted"], lw=0.8),
             Patch(color=st["muted"], alpha=0.25, lw=0), Line2D([], [], color=st["muted"], lw=0.8, ls=":")]
        l = ["Excitation", "Emission", "Detected emission", "Pre state: excitation, emission (PALM)", "Laser line (nm)",
             "Dichroic transmission", "Emission filter transmission", "Camera QE"]
        fig.legend(h, l, loc="outside lower center", ncol=4, fontsize=6.5,
                   handler_map={tuple: HandlerTuple(ndivide=None, pad=0.6)})

    body = ctx.plot("", draw, h=5.1, alt="Dye spectra through the light path")
    return figure(body,
                  "The typical microtubule label of each mode with its light preset, as a movie resolves it (the "
                  "cli's setup output). The detected part of the emission passes the dichroic, the emission filter "
                  "and the camera's QE; F is its fraction. mEos3.2's green pre state (before photoconversion) is "
                  "mostly blocked by the PALM filter. Photons per second: a dye while it emits, after the "
                  "objective's collection efficiency.")


def movie(ctx, opts, tag, frames=50, size=96, photons=True):
    r = ctx.cli(dict(SPOT, size=size, frames=frames, **opts), tag=tag, out=True, photons_out=photons, setup_json=True)
    return stack(r["out"]), stack(r["photons_out"]) if photons else None, load(r["setup_json"]), r


@register("light-modes", "dyes-and-light-path")
def light_modes(ctx):
    cols, frames = [], []
    for m in MODES:
        adu, _, setup, _ = movie(ctx, typical(m), m, frames=1, photons=False)
        lab = setup["labels"][0]
        frames.append(gray(ctx, m + "-frame", adu[0]))
        cols.append("%s<br>%s, %g %%" % (m, lab["dye_name"], lab["labelled_pct"]))
    return table(cols, [[img(p) for p in frames]], label_col=False) + \
        "\nOne camera frame of the same 9.6 µm field (96 px of 100 nm, 50 ms, t = 60 s) with each mode's typical " \
        "label, labelled share and light preset, each with its own contrast. The blinking modes show a few emitters " \
        "per frame and the structure only over many frames; WideField shows it in every frame.\n"


@register("light-shutters", "dyes-and-light-path")
def light_shutters(ctx):
    cases = [("lasers", 1, 0), ("lamp", 0, 1), ("both", 1, 1), ("none", 0, 0)]
    tiles, med = [], []
    for name, epi, trans in cases:
        r = ctx.cli(dict(SPOT, size=96, frames=1, **{"light-epi": epi, "light-trans": trans,
                                                    "bf-photons-per-px-per-sec": 5000}), tag=name, out=True)
        a = stack(r["out"])[0]
        tiles.append(gray(ctx, name, a))
        med.append("%.0f" % np.median(a))
    return table(["", "Lasers (epi)", "Lamp (transmitted)", "Both", "None"],
                 [["Frame"] + [img(t) for t in tiles], ["Median (ADU)"] + med]) + \
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
    return table(["Imager"] + ["%g nM" % c for c in concs],
                 [["Frame"] + [img(t) for t in tiles],
                  ["Background, model (photons/px/frame)"] + [sig(v, 3) for v in model],
                  ["Median, rendered (photons/px/frame)"] + [sig(v, 3) for v in measured]]) + \
        "\nThe DNA-PAINT imager concentration sets the binding rate (k<sub>on</sub>c: more blinks) and the free " \
        "imager's flat background c N<sub>A</sub> H A<sub>px</sub> k<sub>em</sub>&eta;F (model row, from the cli's " \
        "setup output). The rendered row is the median of the photon images before the camera, which also holds " \
        "the blinks' halos. Frames: one camera frame (ADU, each its own contrast). Default 1 nM; frames 50 ms.\n"


@register("light-meanfield", "dyes-and-light-path", timed=True)
def light_meanfield(ctx):
    frames = 20
    base = dict(SPOT, size=64, frames=frames, **typical("WideField"), **{"mt-label-pct": 10})
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

    def per_frame(t):
        return secs((t[1] - t[0]) / frames)

    return table(["", "Per dye", "Mean field", "Mean field - per dye"],
                 [["Photons", img(ctx.tile("perdye", pd, vmin=0, vmax=vmax)),
                   img(ctx.tile("meanfield", mf, vmin=0, vmax=vmax)), img(ctx.diff_tile("diff", d, scale))],
                  ["How", "Each dye's window splatted", "Dye density on z planes &times; PSF",
                   "&plusmn;%s photons = %s %% of the mean" % (sig(scale, 2), sig(100 * scale / pd.mean(), 2))],
                  ["Total photons", sig(pd.sum(), 4), sig(mf.sum(), 4), "%+.2f %%" % (100 * (mf.sum() / pd.sum() - 1))],
                  ["rms difference per pixel", "", "", "%s %% of the mean" % sig(100 * np.sqrt(np.mean(d ** 2)) / pd.mean(), 2)],
                  ["Setup / frame", "%s / %s" % (secs(t_pd[0]), per_frame(t_pd)),
                   "%s / %s" % (secs(t_mf[0]), per_frame(t_mf)), ""]]) + \
        "\nA continuous population (mEGFP WideField, 10 %% of the sites labelled, 64 px, 50 ms frames; the images " \
        "are the first frame) rendered both ways, forced by the switch's thresholds; by default the renderer picks " \
        "mean field above 20 emitting dyes per µm<sup>2</sup> of the 500 nm focal slab " \
        "(`mean-field-density-per-um2`). Times on the build machine (%d threads): the setup in a fresh process, " \
        "the frame time averaged over %d frames.\n" % (BUILD_INFO.get("cores", 0), frames)


@register("light-blink-regimes", "dyes-and-light-path", timed=True)
def light_blink_regimes(ctx):
    frames, size = 20, 64
    base = dict(SPOT, size=size, frames=frames, **typical("DNA-PAINT"))
    regimes = (("Splat (SMLM)", {"blink-binned-density-per-um2": 1e9}),
               ("Binned (approximate SMLM)", {"blink-binned-max-emitters": 0}),
               ("Mean field", {"blink-mean-field-max-emitters": 0}))
    rows, slab_um = [], 0.5
    for nm, level in ((1, "Low"), (10, "Medium"), (100, "High")):
        dens = dict(base, **{"mt-imager-nm": nm})
        got = []
        for name, opts in regimes:
            r = ctx.cli(dict(dens, **opts), tag="%gnM-%s" % (nm, name.split()[0].lower()), out=True, photons_out=True,
                        setup_json=True)
            st = r.time("out")
            got.append((stack(r["out"]).astype(np.float64), stack(r["photons_out"]), (st[1] - st[0]) / frames))
        fr = load(r["setup_json"])["frame"]
        t0, t1 = fr["start_s"], fr["start_s"] + frames * fr["exposure_s"]
        # The ON emitters per frame (overlap-weighted, what the splat draws) in the field of view: over all depths (the
        # binned rule's density) and within the focal slab (the mean-field rule's, there as an expectation).
        d = load(ctx.cli(dict(dens, **{"dyes-t": [t0, t1]}), tag="%gnM-dyes" % nm, dyes_json=True)["dyes_json"])
        e = np.asarray(d["events"], dtype=np.float64).reshape(-1, 10)
        x0, y0, x1, y1 = d["rect"]
        ov = np.clip(np.minimum(e[:, 4], t1) - np.maximum(e[:, 3], t0), 0, None) / fr["exposure_s"]
        area = (x1 - x0) * (y1 - y0)
        on_fov = ov.sum() / frames / area
        on_slab = ov[np.abs(e[:, 2] - fr["focus_um"]) < slab_um / 2].sum() / frames / area
        dflt = stack(ctx.cli(dens, tag="%gnM-default" % nm, photons_out=True)["photons_out"])
        picked = next((name for (name, _), g in zip(regimes, got) if np.array_equal(dflt, g[1])), "?")
        lo, hi = robust_range(np.concatenate([g[0] for g in got]), 0.5, 99.9)
        p0 = got[0][1]
        cells, stats = [], []
        for (name, _), (adu, ph, t) in zip(regimes, got):
            cells.append(img(ctx.gif("%gnM-%s" % (nm, name.split()[0].lower()), adu, vmin=lo, vmax=hi, min_px=192)))
            fluct = float(ph.std(axis=0).mean() / max(ph.mean(), 1e-12))
            stats.append("%s / frame<br>photons %+.1f %%<br>frame-to-frame std %s %% of the mean" % (
                secs(t), 100 * (ph.sum() / p0.sum() - 1), sig(100 * fluct, 2)))
        diff = got[1][1][0] - p0[0]
        scale = float(np.abs(diff).max())
        cells.append(img(ctx.diff_tile("%gnM-diff" % nm, diff, scale, min_px=192)))
        stats.append("&plusmn;%s photons = %s %% of the splat frame's peak<br>rms %s %% of its mean" % (
            sig(scale, 2), sig(100 * scale / float(p0[0].max()), 2),
            sig(100 * float(np.sqrt(np.mean(diff ** 2))) / float(p0[0].mean()), 2)))
        rows.append(["<b>%s</b>: %g nM imager<br>%s ON / &micro;m<sup>2</sup> (all z)<br>%s ON / &micro;m<sup>2</sup> "
                     "in the focal slab<br>default: %s" % (level, nm, sig(on_fov, 2), sig(on_slab, 2),
                                                           picked.split(" (")[0].lower())] + cells)
        rows.append([""] + stats)
    return table(["", "Splat (SMLM)", "Binned (approximate SMLM)", "Mean field", "Binned &minus; splat (frame 0)"],
                 rows) + \
        "\nDNA-PAINT (ATTO 655 imager, %d px, %d frames of 50 ms) at three imager concentrations, each rendered in the " \
        "three blink regimes, forced by their thresholds (one movie per cell, camera ADU on one scale per row). " \
        "Splat and binned draw the same blinks; binned snaps each to a cell of half a pixel (&plusmn;25 nm), and the " \
        "last column is what that moves: the photon images (before the camera's noise) of frame 0, binned minus " \
        "splat, red where binned has more light, blue where less, on &plusmn; the largest difference. Mean field draws " \
        "no blinks, only each dye's expected ON time, so its frames hold still apart from shot noise.\n\n" \
        "*ON / &micro;m<sup>2</sup>*: the ON emitters per frame (each blink weighted by the part of the frame it is " \
        "ON) per &micro;m<sup>2</sup> of the field of view, over all depths and within the %d nm focal slab " \
        "(`mean-field-slab-nm`), averaged over the movie. *default* is the regime the renderer picks by itself: " \
        "binned while more than 10 are ON per &micro;m<sup>2</sup> of the field of view (`blink-binned-density-per-um2`; " \
        "the rule counts the blinks the movie queries, which reach a little beyond the field of view), splat below. " \
        "Mean field is never picked by default: it takes over only when you set `blink-mean-field-density-per-um2` " \
        "below the expected ON density in the focal slab (or `blink-mean-field-max-emitters` below the expected ON " \
        "count over all depths); both are 1e9 by default, because mean field loses the blinking. Photons: the " \
        "movie's total against the splat's; frame-to-frame std: of the photon images, per pixel, averaged (0 for " \
        "mean field, whose expected image stays put). Times on the build machine (%d threads), per frame.\n" % (
            size, frames, round(1000 * slab_um), BUILD_INFO.get("cores", 0))
