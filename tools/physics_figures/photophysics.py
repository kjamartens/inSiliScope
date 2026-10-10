"""Photophysics page: emission traces of single dyes per mode with each mode's state diagram, the dSTORM initial ON
phase, WideField bleaching, per-blink brightness and the rate history (a change acts from now on)."""
import numpy as np
from matplotlib.patches import Patch

from .common import COLORS, SPOT, figure, img, load, register, robust_range, rows, sig, stack, table
from .light import MODES, typical

BLINK, STEADY, PRE = COLORS[3], COLORS[0], COLORS[2]   # the traces' colours: blinks, initial ON / always on, pre state


def dyes(ctx, opts, tag, rect, t0, t1):
    r = ctx.cli(dict(SPOT, **opts, **{"dyes-rect": rect, "dyes-t": [t0, t1]}), tag=tag, dyes_json=True,
                setup_json=True)
    d, s = load(r["dyes_json"]), load(r["setup_json"])
    return rows(d["events"], 10), rows(d["continuous"], 10), rows(d["sites"], 5), s


# Each mode's states and transitions as the core schedules them (docs/physics/photophysics.md): nodes (x, y, label,
# colour key), edges (from, to, label, curvature). Colour keys: the traces' colours, or None (dark, bleached).
DIAGRAMS = {
    "dSTORM": ({"init": (0.2, 0.82, "Initial ON", STEADY), "dark": (0.2, 0.18, "Dark", None),
                "on": (1.0, 0.18, "ON", BLINK), "bl": (1.0, 0.82, "Bleached", None)},
               [("init", "dark", r"$\tau_{init}$", 0), ("dark", "on", r"$k_{act}$, $1/\tau_{off}$", -0.35),
                ("on", "dark", r"$1/\tau_{on}$", -0.35), ("on", "bl", r"$p_b$", 0)]),
    # PALM: the pre state converts to ON (k_act) or bleaches unconverted (B_pre); converted, it blinks like dSTORM.
    "PALM": ({"pre": (0.08, 0.8, "Pre state", PRE), "on": (0.66, 0.8, "ON", BLINK), "dark": (1.2, 0.8, "Dark", None),
              "bl": (0.66, 0.2, "Bleached", None)},
             [("pre", "on", r"$k_{act}$", 0), ("pre", "bl", r"$B_{pre}$", 0), ("on", "dark", r"$1/\tau_{on}$", -0.45),
              ("dark", "on", r"$1/\tau_{off}$", -0.45), ("on", "bl", r"$p_b$", 0)]),
    "DNA-PAINT": ({"free": (0.2, 0.5, "Empty site", None), "on": (1.0, 0.5, "Imager\nbound (ON)", BLINK)},
                  [("free", "on", r"$k_{on}c$", -0.45), ("on", "free", r"$1/\tau_{on}$", -0.45)]),
    "WideField": ({"on": (0.2, 0.5, "Emitting", STEADY), "bl": (1.0, 0.5, "Bleached", None)},
                  [("on", "bl", r"$\lambda = k_{em}/B$", 0)]),
}


def state_diagram(ax, st, mode):
    nodes, edges = DIAGRAMS[mode]
    ax.set_xlim(-0.12, 1.32)
    ax.set_ylim(-0.02, 1.02)
    ax.set_aspect("equal")
    ax.axis("off")
    boxes = {}
    for k, (x, y, label, c) in nodes.items():
        boxes[k] = ax.text(x, y, label, ha="center", va="center", fontsize=6.3, color=st["fg"], zorder=3,
                           bbox=dict(boxstyle="round,pad=0.35", fc=c if c else st["grid"], ec=c if c else st["muted"],
                                     lw=0.6, alpha=0.55 if c else 0.6))
    for a, b, label, rad in edges:
        (xa, ya), (xb, yb) = nodes[a][:2], nodes[b][:2]
        ax.annotate("", xy=(xb, yb), xytext=(xa, ya), zorder=2,
                    arrowprops=dict(arrowstyle="-|>", color=st["muted"], lw=0.7, mutation_scale=7, shrinkA=1.5,
                                    shrinkB=1.5, connectionstyle="arc3,rad=%g" % rad,
                                    patchA=boxes[a].get_bbox_patch(), patchB=boxes[b].get_bbox_patch()))
        # The label beside the arc's middle (arc3: the curve's midpoint is rad/2 x the chord turned by -90 degrees).
        dx, dy = xb - xa, yb - ya
        mx, my = (xa + xb) / 2 + rad / 2 * dy, (ya + yb) / 2 - rad / 2 * dx
        n = np.hypot(dx, dy)
        side = np.sign(rad) if rad else 1.0
        off = 0.09 if rad else 0.12   # a straight edge's label clears the arrow head's line
        ox, oy = side * dy / n * off, -side * dx / n * off
        ax.text(mx + ox, my + oy, label, ha="center", va="center", fontsize=6.5, color=st["fg"], zorder=4)


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
        gs = fig.add_gridspec(len(MODES), 2, width_ratios=[1, 3.3])
        axs = []
        for row, m in enumerate(MODES):
            state_diagram(fig.add_subplot(gs[row, 0]), st, m)
            ax = fig.add_subplot(gs[row, 1])
            axs.append(ax)
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
                    ax.broken_barh([(a, min(b, T) - a)], (k - 0.35, 0.7), color=PRE if s == 1 else STEADY)
                em = ev[:, 6] == i
                for a, b, br in zip(ev[em, 3], ev[em, 4], ev[em, 5]):
                    ax.broken_barh([(a, max(b - a, T / 600))], (k - 0.4, 0.8), color=BLINK,
                                   alpha=float(np.clip(0.35 + 0.4 * br, 0.35, 1)))
            ax.set_xlim(0, T)
            ax.set_ylim(-0.8, max(len(ids), 1) - 0.2)
            ax.set_yticks([])
            ax.set_ylabel("%s\n%s" % (m, lab["dye_name"]), fontsize=7)
        axs[-1].set_xlabel("Time since the light came on (s); WideField: 0-%g s" % windows["WideField"])
        fig.legend([Patch(color=BLINK), Patch(color=STEADY), Patch(color=PRE)],
                   ["Blink (shade: brightness)", "Initial ON / always on", "PALM pre state"],
                   loc="outside lower center", ncol=3)

    body = ctx.plot("", draw, h=6.0, alt="State diagrams and emission traces of single dyes per mode")
    return figure(body,
                  "Left: each mode's states and the rates between them, as the core schedules them (the rates "
                  "below). Right: when single dyes emit, twelve dyes per mode (rows), drawn from the core's event "
                  "and continuous window queries of a 1.2 µm square, with each mode's typical label and light preset. "
                  "dSTORM dyes start in the initial ON phase, then blink until they bleach; PALM proteins emit green "
                  "(pre state) until activated, then blink; DNA-PAINT sites blink for ever at k<sub>on</sub>c; "
                  "WideField dyes emit until their photon budget is spent (note the longer time axis). Dyes that "
                  "blink in the window are shown first.")


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
        ax.semilogy(t, total / dt, color=BLINK)
        for s in show:
            ax.axvline(s + dt / 2, color=st["grid"], lw=0.8)
        ax.set_xlabel("Time since the light came on (s)")
        ax.set_ylabel("Detected photons/s in the FOV")

    curve = ctx.plot("curve", draw, h=2.0, alt="dSTORM photons over time from the light coming on")
    return table(["t = %g s" % s for s in show], [[img(x) for x in tiles]], label_col=False) + "\n" + \
        figure(curve, "AF647 dSTORM (3 %% labelled, its light preset) from the moment the light comes on: every dye "
                      "starts in the initial ON phase (all on, the structure plain to see), then switches off and "
                      "blinks. Photon images before the camera, 250 ms frames, each with its own contrast (square-root "
                      "scale); the curve sums each frame's photons. cli and viewer movies start at 60 s by default "
                      "(`start-sec`), past this phase.")


@register("photo-wf-bleach", "photophysics")
def photo_wf_bleach(ctx):
    dt = 5.0
    opts = dict(SPOT, size=48, **typical("WideField"), **{"start-sec": 0, "exposure-ms": dt * 1000, "frames": 40})
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
        ax.axhline(0, color=st["muted"], lw=0.8, ls=":")
        ax.plot(t, total / total[0], color=COLORS[2], label="Rendered (sum of each frame's photons)")
        ax.plot(t, np.exp(-np.log(2) * (t - t[0]) / half_model), color=st["muted"], ls="--",
                label="exp(-t ln2 / t$_{1/2}$), t$_{1/2}$ = ln2 B / k$_{em}$")
        ax.axvline(half_model, color=st["grid"], lw=0.8)
        ax.set_xlim(0, t[-1] + dt / 2)
        ax.set_ylim(-0.05, 1.08)
        ax.set_xlabel("Time since the light came on (s)")
        ax.set_ylabel("Photons / first frame")
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
    bins = np.linspace(0, 3.5, 71)

    def draw(fig, st):
        ax = fig.subplots()
        b = data[0.5]
        ax.hist(b, bins=bins, color=BLINK, alpha=0.8,
                label="photon-cv 0.5 (default): mean %.3f, CV %.3f, %d blinks" % (b.mean(), b.std() / b.mean(), len(b)))
        # The model the core draws from: log-normal with mean 1 and CV c (sigma^2 = ln(1 + c^2), mu = -sigma^2/2),
        # as expected counts per bin.
        s2 = np.log(1 + 0.5 ** 2)
        xs = np.linspace(1e-3, bins[-1], 400)
        pdf = np.exp(-(np.log(xs) + s2 / 2) ** 2 / (2 * s2)) / (xs * np.sqrt(2 * np.pi * s2))
        ax.plot(xs, pdf * len(b) * (bins[1] - bins[0]), color=st["fg"], lw=1.0,
                label="Log-normal, mean 1, CV 0.5 (the model)")
        b0 = data[0.0]
        ax.axvline(1.0, color=COLORS[1], lw=1.5,
                   label="photon-cv 0: every blink %.3g (%d blinks)" % (np.median(b0), len(b0)))
        ax.set_xlabel("Blink brightness (× the dye's detected rate)")
        ax.set_ylabel("Blinks")
        ax.legend()

    body = ctx.plot("", draw, h=2.2, alt="Per-blink brightness histogram")
    return figure(body,
                  "Per-blink brightness of the default label (ATTO 655 DNA-PAINT) over 30 s in a 4 µm square, from the "
                  "core's event query: log-normal with mean 1 and coefficient of variation `photon-cv` (the line: the "
                  "model's density scaled to the number of blinks); at 0 every blink is equally bright.")


def total_rate(ctx, opts, tag, past=None):
    """Detected photons per second in the FOV, per frame (--photons-out), and the frames' mid times on the clock."""
    o = dict(opts)
    if past:
        o["history-before"] = ",".join("%s=%s" % kv for kv in past.items())
    r = ctx.cli(o, tag=tag, photons_out=True)
    ph = stack(r["photons_out"])
    dt = o["exposure-ms"] / 1000
    return o["start-sec"] + (np.arange(len(ph)) + 0.5) * dt, ph.sum(axis=(1, 2)) / dt


@register("photo-history", "photophysics")
def photo_history(ctx):
    cases = []
    # PALM: 405 nm off at 20 s. WideField: 488 nm x4 at 30 s. The power each preset sets comes from the setup output.
    for mode, nm, factor, t_sw, t_end, dt in (("PALM", 405, 0.0, 20.0, 40.0, 0.5),
                                              ("WideField", 488, 4.0, 30.0, 100.0, 1.0)):
        base = dict(SPOT, size=64, **typical(mode), **{"exposure-ms": dt * 1000})
        s = load(ctx.cli(base, tag=mode + "-setup", setup_json=True)["setup_json"])
        p0 = next(L["kw_per_cm2"] for L in s["light_path"]["lasers"] if L["nm"] == nm)
        p1 = p0 * factor
        n_all, n_after = int(round(t_end / dt)), int(round((t_end - t_sw) / dt))
        before = total_rate(ctx, dict(base, **{"start-sec": 0, "frames": n_all}), mode + "-unchanged")
        after = total_rate(ctx, dict(base, **{"start-sec": t_sw, "frames": n_after, "laser-%d" % nm: p1}),
                           mode + "-history", past={"laser-%d" % nm: p0})
        reread = total_rate(ctx, dict(base, **{"start-sec": t_sw, "frames": n_after, "laser-%d" % nm: p1}),
                            mode + "-reread")
        cases.append((mode, s["labels"][0]["dye_name"], nm, p0, p1, t_sw, before, after, reread))

    def draw(fig, st):
        axs = fig.subplots(1, 2)
        for ax, (mode, dye, nm, p0, p1, t_sw, before, after, reread) in zip(axs, cases):
            change = "%d nm off" % nm if p1 == 0 else "%d nm × %g" % (nm, p1 / p0)
            ax.axhline(0, color=st["muted"], lw=0.8, ls=":")
            ax.axvline(t_sw, color=st["grid"], lw=0.8)
            top = max(before[1].max(), after[1].max(), reread[1].max())
            e = int(np.floor(np.log10(top))) if top > 0 else 0
            u = 10.0 ** e
            ax.plot(before[0], before[1] / u, color=st["muted"], lw=1.0, label="Light unchanged")
            n = int(np.searchsorted(before[0], t_sw))
            ax.plot(np.r_[before[0][:n], after[0]], np.r_[before[1][:n], after[1]] / u, color=COLORS[3], lw=1.3,
                    label="Changed at the grey line: continues from the present state (rate history)")
            ax.plot(reread[0], reread[1] / u, color=COLORS[1], lw=1.0, ls="--",
                    label="Changed, read from clock 0 (no history, as before 2026-10-08)")
            ax.set_title("%s %s: %s at %g s" % (dye, mode, change, t_sw), fontsize=8)
            ax.set_xlabel("Lit time (s)")
            ax.set_ylabel("Detected photons/s in the FOV ($10^{%d}$)" % e)
            ax.set_xlim(0, before[0][-1] + (before[0][1] - before[0][0]) / 2)
        h, l = axs[0].get_legend_handles_labels()
        fig.legend(h, l, loc="outside lower center", ncol=2, fontsize=6.5, frameon=False)

    body = ctx.plot("", draw, h=3.0, alt="A rate change acts from now on")
    (m0, d0, nm0, a0, b0, t0, *_), (m1, d1, nm1, a1, b1, t1, *_) = cases
    return figure(body,
                  "The rate history (Micro-Manager's illumination history), reproduced with the cli's "
                  "`--history-before`: the sample has been lit since clock 0, the light changes at the grey line. "
                  "Left: %s %s, 405 nm switched off at %g s (%s kW/cm<sup>2</sup> to 0): the proteins converted so far "
                  "blink out within seconds, no new ones follow. Right: %s %s, 488 nm from %s to %s kW/cm<sup>2</sup> "
                  "at %g s: the next frame is about 4x as bright and the field bleaches 4x as fast from what is left. "
                  "Dashed: the new setting applied to the whole past instead, as if it had been on from the start. "
                  "Photons before the camera, 64 px, frames of 0.5 s (PALM) and 1 s (WideField)."
                  % (d0, m0, t0, sig(a0, 2), d1, m1, sig(a1, 2), sig(b1, 2), t1))
