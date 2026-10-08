"""Camera and background page: the noise chain step by step, photon transfer, camera presets, the per-pixel gain
spread on BrightField, and the drift presets."""
import numpy as np

from .common import COLORS, SPOT, figure, img, load, read_drift, register, robust_range, sig, stack, table
from .light import typical

IDEAL = {"gain": 1, "read-noise": 0, "read-noise-std-pct": 0, "gain-std-pct": 0, "offset": 0, "offset-std": 0,
         "dark-per-sec": 0}


def electrons(adu, cam):
    """ADU back to (photo)electrons with the camera's offset and gain (as set up: setup-json camera)."""
    return (adu - cam["offset_adu"]) * cam["gain_e_per_adu"]


@register("cam-chain", "camera")
def cam_chain(ctx):
    base = dict(SPOT, size=64, frames=1)
    r = ctx.cli(base, tag="photons", photons_out=True)
    ph = stack(r["photons_out"])[0]
    steps = [("Shot noise only", dict(IDEAL)),
             ("+ Dark current, read noise", dict(IDEAL, **{"read-noise": 1.2, "read-noise-std-pct": 20,
                                                          "dark-per-sec": 1.03})),
             ("Kinetix22 (default)", {}),
             ("iXon Ultra 897 (EMCCD)", {"camera-preset": "iXonUltra897"})]
    frames = [("Expected photons", ph)]
    for k, (name, opts) in enumerate(steps):
        r = ctx.cli(dict(base, **opts), tag="step%d" % (k + 1), out=True, setup_json=True)
        frames.append((name, electrons(stack(r["out"])[0], load(r["setup_json"])["camera"])))
    iy, ix = np.unravel_index(np.argmax(ph), ph.shape)
    lo, hi = robust_range(ph, 0.5, 99.9)
    tiles = [img(ctx.tile("step%d" % k, e, cmap="gray", vmin=lo, vmax=hi)) for k, (_, e) in enumerate(frames)]

    def draw(fig, st):
        ax = fig.subplots()
        for k, (name, e) in enumerate(frames):
            ax.plot(np.arange(-12, 13), e[iy, ix - 12:ix + 13] if 12 <= ix < e.shape[1] - 12 else
                    np.interp(np.arange(-12, 13), np.arange(e.shape[1]) - ix, e[iy]),
                    color=st["fg"] if k == 0 else COLORS[k - 1], lw=1.6 if k == 0 else 0.9, label=name)
        ax.set_xlabel("Pixels from the brightest pixel, along its row")
        ax.set_ylabel("Photons / photoelectrons")
        ax.legend(fontsize=6.5)

    prof = ctx.plot("profile", draw, h=2.0, alt="One row through a blink at each step")
    return table([n for n, _ in frames], [tiles], label_col=False) + "\n" + \
        figure(prof, "The camera's noise chain switched on step by step, on one frame of the default movie (ATTO 655 "
                     "DNA-PAINT, 64 px). Every panel in photoelectrons on one grey scale ((ADU - offset) x gain): the "
                     "expected photons (the photon image before the camera), Poisson shot noise only, plus dark "
                     "current and read noise (with its per-pixel spread), the full Kinetix22 (gain, offset, per-pixel "
                     "gain spread, 16 bits) and the iXon Ultra 897 EMCCD (EM gain with its excess noise, "
                     "clock-induced charge). Below: the row through the brightest pixel.")


@register("cam-ptc", "camera")
def cam_ptc(ctx):
    # Flat backgrounds (photons/px per 50 ms frame, before the QE) per camera, below its 16-bit ceiling.
    cams = {"Kinetix22": [10, 30, 100, 300, 1e3, 3e3, 1e4, 3e4],
            "iXonUltra897": [1, 3, 10, 30, 100, 200]}
    res = {}
    for cam, levels in cams.items():
        pts = []
        for bg in levels:
            r = ctx.cli(dict(size=64, frames=2, occupancy=0, **{"psf-model": 0, "camera-preset": cam,
                                                                "background-per-sec": bg * 20}),
                        tag="%s-%g" % (cam, bg), out=True, setup_json=True)
            f = stack(r["out"])
            c = load(r["setup_json"])["camera"]
            if (f >= 65535).any():
                continue   # clipped at the 16-bit ceiling: no longer shot-noise limited
            pts.append((float(f.mean() - c["offset_adu"]), float(np.var(f[1] - f[0]) / 2)))
        res[cam] = (np.array(pts), c)

    def draw(fig, st):
        ax = fig.subplots()
        for k, (cam, (p, c)) in enumerate(res.items()):
            g = c["gain_e_per_adu"]
            enf2 = 2.0 if c["emccd"] else 1.0
            ax.loglog(p[:, 0], p[:, 1], "o", ms=4, color=COLORS[k], label="%s, measured" % cam)
            top = p[-3:]
            slope = float(np.polyfit(top[:, 0], top[:, 1], 1)[0])
            xs = np.logspace(np.log10(p[:, 0].min()), np.log10(p[:, 0].max()), 50)
            ax.loglog(xs, enf2 * xs / g, "--", color=COLORS[k], lw=0.8,
                      label="ENF$^2$/gain = %s ADU (fitted: %s)" % (sig(enf2 / g, 3), sig(slope, 3)))
        ax.set_xlabel("Mean signal (ADU above the offset)")
        ax.set_ylabel("Temporal variance (ADU$^2$)")
        ax.legend(fontsize=6.5)

    body = ctx.plot("", draw, h=2.6, alt="Photon transfer curves")
    return figure(body,
                  "Photon transfer of the two camera presets, measured on the cli's frames: an empty field "
                  "(occupancy 0) under a flat background of increasing intensity, the variance from the difference "
                  "of two frames (the per-pixel offset and gain patterns cancel). Shot noise makes the variance grow "
                  "with the mean at 1/gain (gain in electrons per ADU); the EMCCD's multiplication doubles that (an "
                  "excess noise factor of &radic;2 [[hirsch2013](../references.md#hirsch2013)]); at low signal "
                  "the read-noise floor shows. The fit is the slope through the three brightest levels; levels "
                  "that reach the 16-bit ceiling are left out. The "
                  "lowest points sit on the free imager's background (the default DNA-PAINT label).")


@register("cam-presets", "camera")
def cam_presets(ctx):
    base = dict(SPOT, size=64, frames=1)
    cams = [("Kinetix22 (sCMOS, default)", {}), ("iXon Ultra 897 (EMCCD)", {"camera-preset": "iXonUltra897"}),
            ("Ideal (QE 1, shot noise only)", dict(IDEAL, **{"camera-preset": "Custom", "qe-curve": "Custom",
                                                             "qe": 1}))]
    cols, tiles, f_row, rn_row, gain_row, snr_row = [], [], [], [], [], []
    for k, (name, opts) in enumerate(cams):
        r = ctx.cli(dict(base, **opts), tag="cam%d" % k, out=True, setup_json=True)
        adu = stack(r["out"])[0]
        s = load(r["setup_json"])
        c, lab = s["camera"], s["labels"][0]
        e = electrons(adu, c)
        lo, hi = robust_range(adu, 0.5, 99.9)
        tiles.append(img(ctx.tile("cam%d" % k, adu, cmap="gray", vmin=lo, vmax=hi)))
        cols.append(name)
        f_row.append("%.2f" % lab["main"]["detected_fraction"])
        rn_row.append("%s e<sup>-</sup>%s" % (sig(c["read_noise_e"], 2),
                                               ", EM gain %s" % sig(c["em_gain"], 3) if c["emccd"] else ""))
        gain_row.append(sig(c["gain_e_per_adu"], 2))
        iy, ix = np.unravel_index(np.argmax(e), e.shape)
        peak = float(e[max(0, iy - 1):iy + 2, max(0, ix - 1):ix + 2].sum())
        dim = np.sort(e.ravel())[: e.size // 2]
        snr_row.append(sig(peak / (3 * float(np.std(dim))), 2))
    return table([""] + cols,
                 [["Frame (ADU)"] + tiles,
                  ["Detected fraction F (holds the QE)"] + f_row,
                  ["Read noise"] + rn_row, ["Gain (e<sup>-</sup>/ADU)"] + gain_row,
                  ["Brightest blink / noise"] + snr_row]) + \
        "\nThe camera presets (`Camera.CameraPreset`, cli `camera-preset`) on one frame of the default movie (ATTO " \
        "655 DNA-PAINT, 64 px), and an ideal camera to compare. Each image its own contrast. The QE curve enters " \
        "through the label's detected fraction F. 'Brightest blink / noise': a rough signal-to-noise measure on the " \
        "frame itself, the brightest blink's 3 &times; 3 px sum over the noise of the dimmer half of the pixels " \
        "(which holds the imager background's shot noise too).\n"


@register("cam-prnu", "camera")
def cam_prnu(ctx):
    base = dict(SPOT, size=96, z=1, modality="BrightField")
    r = ctx.cli(dict(base, frames=1), tag="photons", photons_out=True)
    ph = stack(r["photons_out"])[0]
    ph = ph / ph.mean()
    lo, hi = robust_range(ph, 0.5, 99.5)
    pad = 0.25 * (hi - lo)
    lo, hi = lo - pad, hi + pad
    spreads = [0.5]
    one, mean, flat1, flat20 = [], [], [], []
    for pct in spreads:
        r = ctx.cli(dict(base, frames=20, **{"gain-std-pct": pct}), tag="g%g" % pct, out=True, setup_json=True)
        off = load(r["setup_json"])["camera"]["offset_adu"]
        f = stack(r["out"]) - off
        for row, a, name in ((one, f[0], "one"), (mean, f.mean(0), "mean")):
            row.append(img(ctx.tile("g%g-%s" % (pct, name), a / a.mean(), cmap="gray", vmin=lo, vmax=hi)))
        e = stack(ctx.cli(dict(base, frames=20, occupancy=0, **{"gain-std-pct": pct}), tag="flat%g" % pct,
                          out=True)["out"]) - off
        flat1.append(sig(100 * float(e[0].std() / e[0].mean()), 2))
        flat20.append(sig(100 * float(e.mean(0).std() / e.mean(0).mean()), 2))
    return table(["", "No noise (photons)"] + ["Gain spread %g %%%s" % (p, " (default)" if p == 0.5 else "")
                                               for p in spreads],
                 [["One frame", img(ctx.tile("photons", ph, cmap="gray", vmin=lo, vmax=hi))] + one,
                  ["Mean of 20 frames", ""] + mean,
                  ["Cells' contrast (rms, % of the mean)", sig(100 * float(ph.std()), 2)] + [""] * len(spreads),
                  ["Empty field: pixel spread, one frame (%)", ""] + flat1,
                  ["... in the mean of 20 frames (%)", ""] + flat20]) + \
        "\nThe sCMOS per-pixel gain spread (PRNU; `Camera.sCMOS_GainStdPctPerPixel`, cli `gain-std-pct`) is a fixed " \
        "pattern proportional to the signal. BrightField cells are weak phase objects: here (focus 1 µm above the " \
        "coverslip, the default lamp of 4000 photons per pixel and frame) their contrast is below one frame's shot " \
        "noise (the empty field's spread in one frame). Averaging frames removes the shot noise but not the pattern, " \
        "so the gain spread sets how far averaging helps: compare the empty field's spread in the mean of 20 frames " \
        "with the cells' contrast. The default 0.5 % is an *estimate*, above a published sCMOS PRNU of 0.06-0.3 % " \
        "rms [[orcaflash4v3-technote](../references.md#orcaflash4v3-technote)]; it was 5 % before 2026-10-01, a " \
        "pattern far above the cells' contrast. One grey scale for all (intensity / its mean).\n"


@register("drift-presets", "camera")
def drift_presets(ctx):
    pr = load(ctx.cli({}, tag="presets", presets_json=True)["presets_json"])["drift"]
    dt = 0.05
    traj = {}
    for d in pr:
        opts = {k: v for k, v in d.items() if k != "name"}
        r = ctx.cli(dict(SPOT, size=16, frames=600, **opts), tag=d["name"], out=True)
        traj[d["name"]] = read_drift(r["out"])
    # A movie of the Extreme preset: mEGFP WideField, so the structure (and its drift) shows in every frame.
    ext = next(d for d in pr if d["name"] == "Extreme")
    n, step = 300, 3
    r = ctx.cli(dict(SPOT, size=96, frames=n, **typical("WideField"), **{k: v for k, v in ext.items() if k != "name"}),
                tag="extreme-movie", out=True)
    mv = stack(r["out"])[::step]
    lo, hi = robust_range(mv, 0.5, 99.8)
    gif = ctx.gif("extreme", mv, cmap="gray", vmin=lo, vmax=hi, fps=1 / (step * dt),
                  labels=["t = %.1f s" % (k * step * dt) for k in range(len(mv))])

    def draw(fig, st):
        a, b = fig.subplots(1, 2, sharey=True)
        for k, (name, t) in enumerate(traj.items()):
            if t is None:
                continue   # Off: no drift, no trajectory file
            sec = t[1:, 0] * dt
            a.plot(sec, np.hypot(t[1:, 1], t[1:, 2]), color=COLORS[k], lw=0.9, label=name)
            b.plot(sec, np.abs(t[1:, 3]), color=COLORS[k], lw=0.9, label=name)
        for ax, what in ((a, "xy"), (b, "z")):
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.set_xlabel("Time (s)")
            ax.set_title("Displacement in %s" % what, fontsize=7.5)
        a.set_ylabel("nm")
        b.legend(ncol=2, fontsize=6.5)

    body = ctx.plot("", draw, h=2.4, alt="Drift trajectories per preset")
    return figure(img(gif, "Extreme drift"),
                  "The Extreme drift preset (250 nm/s directed, 50 nm/&radic;s random walk, xy and z) on mEGFP "
                  "WideField, 96 px (9.6 µm), %g s of 50 ms frames, every %drd frame, in real time: the field "
                  "slides and the focus wanders into the cell. One grey scale for all frames." % (n * dt, step),
                  max_width="24rem") + "\n" + \
        figure(body, "`SampleHolder.DriftPreset` (viewer: Drift; cli: the speeds and walks it sets, from the cli's "
                     "preset table): how far the sample has moved after each 50 ms frame over 30 s (the cli's "
                     "`.drift.csv`, the true drift; seed 42, the direction and its wander drawn per seed). On log axes "
                     "the random walk grows as &radic;t at first and the directed part as t later.")
