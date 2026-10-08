"""Photophysics page: emission traces of single dyes per mode, the dSTORM initial ON phase, WideField bleaching
and per-blink brightness."""
import numpy as np

from .common import COLORS, SPOT, figure, img, load, register, robust_range, rows, sig, stack, table
from .light import MODES, typical

STATE_NAMES = {0: "blink", 1: "pre state", 2: "initial ON", 3: "always on"}   # ISC_STATE_* (insiliscope.h)


def dyes(ctx, opts, tag, rect, t0, t1):
    r = ctx.cli(dict(SPOT, **opts, **{"dyes-rect": rect, "dyes-t": [t0, t1]}), tag=tag, dyes_json=True,
                setup_json=True)
    d, s = load(r["dyes_json"]), load(r["setup_json"])
    return rows(d["events"], 10), rows(d["continuous"], 10), rows(d["sites"], 5), s


@register("photo-traces", "photophysics")
def photo_traces(ctx):
    x, y = SPOT["x"], SPOT["y"]
    rect = [x - 0.6, y - 0.6, x + 0.6, y + 0.6]
    windows = {"dSTORM": 40.0, "PALM": 40.0, "DNA-PAINT": 40.0, "WideField": 150.0}
    data = {}
    for m in MODES:
        ev, cont, sites, setup = dyes(ctx, typical(m), m, rect, 0, windows[m])
        lab = setup["labels"][0]
        # The bleach end of a continuous window, as the imaging side applies it: aux x photon budget / emission rate.
        ends = cont[:, 4].copy() if len(cont) else np.zeros(0)
        if len(cont):
            if m == "WideField" and lab["photon_budget"] > 0:
                ends = np.minimum(ends, cont[:, 9] * lab["photon_budget"] / lab["main"]["emission_per_s"])
            if m == "PALM" and lab["pre"].get("emits") and lab["pre_photon_budget"] > 0:
                ends = np.minimum(ends, cont[:, 9] * lab["pre_photon_budget"] / lab["pre"]["emission_per_s"])
        data[m] = (ev, cont, ends, sites, lab)

    def draw(fig, st):
        axs = fig.subplots(len(MODES), 1)
        for ax, m in zip(axs, MODES):
            ev, cont, ends, sites, lab = data[m]
            T = windows[m]
            # Which dyes to show (the traces themselves are the cli's): up to 12, those that blink in the window
            # first, in order of their first blink; then others.
            blinking = list(dict.fromkeys(ev[np.argsort(ev[:, 3]), 6])) if len(ev) else []
            rest = [i for i in dict.fromkeys(cont[:, 6]) if i not in set(blinking)] if len(cont) else []
            pick = np.random.default_rng(1)
            if len(blinking) > 12:
                blinking = sorted(pick.choice(blinking, 12, replace=False), key=blinking.index)
            ids = blinking + list(pick.choice(rest, min(len(rest), 12 - len(blinking)), replace=False) if rest else [])
            for k, i in enumerate(ids):
                cm = cont[:, 6] == i
                for a, b, s in zip(cont[cm, 3], ends[cm], cont[cm, 8]):
                    ax.broken_barh([(a, min(b, T) - a)], (k - 0.35, 0.7), color=COLORS[2] if s == 1 else COLORS[0])
                em = ev[:, 6] == i
                for a, b, br in zip(ev[em, 3], ev[em, 4], ev[em, 5]):
                    ax.broken_barh([(a, max(b - a, T / 600))], (k - 0.4, 0.8), color=COLORS[3],
                                   alpha=float(np.clip(0.35 + 0.4 * br, 0.35, 1)))
            ax.set_xlim(0, T)
            ax.set_ylim(-0.8, max(len(ids), 1) - 0.2)
            ax.set_yticks([])
            ax.set_ylabel("%s\n%s" % (m, lab["dye_name"]), fontsize=7)
        axs[-1].set_xlabel("time since the light came on (s); WideField: 0-%g s" % windows["WideField"])
        from matplotlib.patches import Patch
        fig.legend([Patch(color=COLORS[3]), Patch(color=COLORS[0]), Patch(color=COLORS[2])],
                   ["blink (shade: brightness)", "initial ON / always on", "PALM pre state"],
                   loc="outside lower center", ncol=3)

    body = ctx.plot("", draw, h=5.2, alt="Emission traces of single dyes per mode")
    return figure(body,
                  "When single dyes emit: twelve dyes per mode (rows), drawn from the core's event and continuous "
                  "window queries of a 1.2 um square, with each mode's typical label and light preset. dSTORM dyes "
                  "start in the initial ON phase, then blink until they bleach; PALM proteins emit green (pre state) "
                  "until activated, then blink; DNA-PAINT sites blink for ever at k<sub>on</sub>c; WideField dyes "
                  "emit until their photon budget is spent (note the longer time axis). Dyes that blink in the "
                  "window are shown first.")


@register("photo-dstorm-start", "photophysics")
def photo_dstorm_start(ctx):
    dt = 0.25
    opts = dict(SPOT, size=64, **typical("dSTORM"), **{"start-sec": 0, "exposure-ms": dt * 1000, "frames": 124})
    r = ctx.cli(opts, tag="curve", photons_out=True)
    ph = stack(r["photons_out"])
    t = (np.arange(len(ph)) + 0.5) * dt
    total = ph.sum(axis=(1, 2))
    show = [0.0, 1.0, 3.0, 10.0, 30.0]
    picks = [int(round(s / dt)) for s in show]
    tiles = [ctx.tile("t%g" % s, np.sqrt(ph[k]), cmap="magma", vmin=0, vmax=np.sqrt(robust_range(ph[k], hi=99.9)[1]))
             for s, k in zip(show, picks)]

    def draw(fig, st):
        ax = fig.subplots()
        ax.semilogy(t, total / dt, color=COLORS[3])
        for s in show:
            ax.axvline(s + dt / 2, color=st["grid"], lw=0.8)
        ax.set_xlabel("time since the light came on (s)")
        ax.set_ylabel("detected photons/s in the FOV")

    curve = ctx.plot("curve", draw, h=2.0, alt="dSTORM photons over time from the light coming on")
    return table(["t = %g s" % s for s in show], [[img(x) for x in tiles]]) + "\n" + \
        figure(curve, "AF647 dSTORM (3 %% labelled, its light preset) from the moment the light comes on: every dye "
                      "starts in the initial ON phase (all on, the structure plain to see), then switches off and "
                      "blinks. Photon images before the camera, 250 ms frames, each with its own contrast (square-root "
                      "scale); the curve sums each frame's photons. cli and viewer movies start at 60 s by default "
                      "(`start-sec`), past this phase.")


@register("photo-wf-bleach", "photophysics")
def photo_wf_bleach(ctx):
    dt = 10.0
    opts = dict(SPOT, size=48, **typical("WideField"), **{"start-sec": 0, "exposure-ms": dt * 1000, "frames": 60})
    r = ctx.cli(opts, tag="wf", photons_out=True, setup_json=True)
    ph = stack(r["photons_out"])
    setup = load(r["setup_json"])
    lab = setup["labels"][0]
    t = (np.arange(len(ph)) + 0.5) * dt
    total = ph.sum(axis=(1, 2)) / dt
    half_model = np.log(2) * lab["photon_budget"] / lab["main"]["emission_per_s"]
    k = int(np.argmax(total < total[0] / 2)) if np.any(total < total[0] / 2) else -1
    half_meas = float(np.interp(total[0] / 2, total[::-1], t[::-1])) if k > 0 else float("nan")

    def draw(fig, st):
        ax = fig.subplots()
        ax.plot(t, total / total[0], color=COLORS[2], label="rendered (sum of each frame's photons)")
        ax.plot(t, np.exp(-np.log(2) * (t - t[0]) / half_model), color=st["muted"], ls="--",
                label="exp(-t ln2 / t$_{1/2}$), t$_{1/2}$ = ln2 B / k$_{em}$")
        ax.axvline(half_model, color=st["grid"], lw=0.8)
        ax.set_xlabel("time since the light came on (s)")
        ax.set_ylabel("photons / first frame")
        ax.legend()

    body = ctx.plot("", draw, h=2.2, alt="WideField bleaching over time")
    return figure(body,
                  "mEGFP WideField (70 %% labelled, its light preset): each dye emits until its photon budget "
                  "B (per dye B x an Exp(1) draw) is spent, so the field fades exponentially with half time "
                  "ln2 B/k<sub>em</sub> = %s s (from the cli's setup output); the rendered frames halve at %s s. "
                  "Frames of %g s." % (sig(half_model, 3), sig(half_meas, 3), dt))


@register("photo-brightness", "photophysics")
def photo_brightness(ctx):
    x, y = SPOT["x"], SPOT["y"]
    rect = [x - 2, y - 2, x + 2, y + 2]
    data = {}
    for cv in (0.5, 0.0):
        ev, _, _, setup = dyes(ctx, {"mt-dye.photon-cv": cv}, "cv%g" % cv, rect, 60, 90)
        data[cv] = ev[:, 5]

    def draw(fig, st):
        ax = fig.subplots()
        b = data[0.5]
        ax.hist(b, bins=np.linspace(0, 3.5, 71), color=COLORS[3], alpha=0.8,
                label="photon-cv 0.5 (default): mean %.3f, CV %.3f, %d blinks" % (b.mean(), b.std() / b.mean(), len(b)))
        b0 = data[0.0]
        ax.axvline(1.0, color=COLORS[1], lw=1.5,
                   label="photon-cv 0: every blink %.3g (%d blinks)" % (np.median(b0), len(b0)))
        ax.set_xlabel("blink brightness (x the dye's detected rate)")
        ax.set_ylabel("blinks")
        ax.legend()

    body = ctx.plot("", draw, h=2.2, alt="Per-blink brightness histogram")
    return figure(body,
                  "Per-blink brightness of the default label (ATTO 655 DNA-PAINT) over 30 s in a 4 um square, from the "
                  "core's event query: log-normal with mean 1 and coefficient of variation `photon-cv`; at 0 every "
                  "blink is equally bright.")
