"""Optics and PSF page: PSF sections and presets, pupil sampling, the halo cut, sub-pixel placement, kernel
oversampling, the WideField pipeline and grid, the BrightField scene, quality and defocus."""
import re

import numpy as np

from .common import (BUILD_INFO, COLORS, SPOT, figure, img, load, pool_times, register, robust_range, secs, sig, stack,
                     table)
from .light import typical


def psf(ctx, opts, tag, fresh=False):
    """The movie's PSF (--psf-out): (planes, camera images, pupil wavefront, json, run)."""
    r = ctx.cli(opts, tag=tag, psf_out=True, fresh=fresh)
    p = r["psf_out"]
    return stack(p + ".planes.tif"), stack(p + ".cams.tif"), stack(p + ".pupil.tif")[0], load(p + ".json"), r


def splat(ctx, opts, tag, dx=0.0, dy=0.0, z=0.0):
    """One 1-photon blink as a movie draws it (--splat-out): (image, json, run)."""
    r = ctx.cli(dict(opts, **{"splat-at": [dx, dy, z]}), tag=tag, splat_out=True)
    return stack(r["splat_out"])[0], load(r["splat_out"] + ".json"), r


def share(x):
    """A share of an emitter's photons in percent ("0" below float rounding)."""
    return "0" if abs(x) < 1e-7 else "%s %%" % sig(100 * x, 2)


def zs(meta):
    """The defocus of each kernel plane (um): plane k sits at (k - (nz-1)/2) z_step."""
    return (np.arange(meta["nz"]) - (meta["nz"] - 1) / 2) * meta["z_step_nm"] / 1000


def xz(planes):
    """The x-z section through the centre (rows = z planes)."""
    return planes[:, planes.shape[1] // 2, :]


def centre(a, half):
    c = a.shape[-1] // 2
    return a[..., c - half:c + half + 1, c - half:c + half + 1]


def log_tile(ctx, name, a, decades=5, ref=None, **kw):
    ref = float(a.max()) if ref is None else ref
    with np.errstate(divide="ignore"):
        v = np.log10(np.maximum(a, 0) / ref)
    return ctx.tile(name, np.maximum(v, -decades - 1), cmap="magma", vmin=-decades, vmax=0, **kw)


def presets(ctx):
    return load(ctx.cli({}, tag="presets", presets_json=True)["presets_json"])


def cores():
    return BUILD_INFO.get("cores", 0)


# ---- PSF ------------------------------------------------------------------------------------------

@register("psf-sections", "optics")
def psf_sections(ctx):
    small = {"psf-kernel-half-width-nm": 2000, "psf-z-range-um": 4}
    data = {}
    for name, opts in (("no aberrations", {"psf-zernike-preset": "None"}),
                       ("default (MixedRealisticObjective)", {})):
        planes, cams, pupil, meta, _ = psf(ctx, dict(small, **opts), name.split()[0])
        data[name] = (planes, meta)
    show_z = [-1.0, -0.5, 0.0, 0.5, 1.0]

    def draw(fig, st):
        gs = fig.add_gridspec(2, 1 + len(show_z), width_ratios=[2.2] + [1] * len(show_z))
        for row, (name, (planes, meta)) in enumerate(data.items()):
            z = zs(meta)
            um = meta["pixel_nm"] / meta["oversampling"] / 1000
            c = planes.shape[-1] // 2
            half = int(round(1.2 / um))
            sec = xz(planes)[:, c - half:c + half + 1]
            ax = fig.add_subplot(gs[row, 0])
            ax.imshow(np.log10(np.maximum(sec / sec.max(), 1e-5)), cmap="magma", vmin=-5, vmax=0, aspect="auto",
                      extent=[-half * um, half * um, z[-1], z[0]])
            ax.set_ylabel("defocus (um)")
            ax.set_title("%s: x-z (log, 5 decades)" % name, fontsize=7.5)
            if row == 1:
                ax.set_xlabel("x (um)")
            h2 = int(round(0.8 / um))
            for k, zz in enumerate(show_z):
                a = fig.add_subplot(gs[row, 1 + k])
                pl = planes[int(np.argmin(np.abs(z - zz)))]
                a.imshow(pl[c - h2:c + h2 + 1, c - h2:c + h2 + 1], cmap="magma")
                a.set_xticks([])
                a.set_yticks([])
                if row == 0:
                    a.set_title("z = %+.1f um" % zz, fontsize=7.5)

    body = ctx.plot("", draw, h=3.4, alt="PSF sections with and without aberrations")
    return figure(body,
                  "The GibsonLanniZernike PSF of the default label (ATTO 655, 678 nm, NA 1.4) as the movie's kernel "
                  "holds it (oversampled 6x). Top: no aberrations, symmetric above and below focus. Bottom: the "
                  "default MixedRealisticObjective preset (defocus, astigmatism, coma and spherical terms): the "
                  "focus is asymmetric and the spot changes shape and shifts with z. Right: x-y planes, 1.6 um "
                  "across, linear, each its own scale.")


@register("psf-presets", "optics")
def psf_presets(ctx):
    pr = presets(ctx)
    small = {"psf-kernel-half-width-nm": 1500, "psf-z-range-um": 2.4, "psf-z-step-um": 0.1}
    cases = [(z["name"], {"psf-zernike-preset": z["name"]}) for z in pr["zernike"]]
    out = []
    for name, opts in cases:
        tag = name.split()[0]
        planes, cams, pupil, meta, _ = psf(ctx, dict(small, **opts), tag)
        z = zs(meta)
        um = meta["pixel_nm"] / meta["oversampling"] / 1000
        h = int(round(1.0 / um))
        c = planes.shape[-1] // 2
        w = float(np.nanmax(np.abs(pupil))) if np.isfinite(pupil).any() else 0.0
        cells = [name,
                 img(ctx.tile(tag + "-pupil", pupil, cmap="RdBu_r", vmin=-w, vmax=w, nan=(128, 128, 128), min_px=96))
                 if w > 0 else "flat",
                 "%.2f" % float(np.sqrt(np.nanmean(pupil ** 2))) if w > 0 else "0",
                 img(log_tile(ctx, tag + "-xz", xz(planes)[:, c - h:c + h + 1], 4, min_px=96))]
        for zz in (-0.6, 0.0, 0.6):
            pl = planes[int(np.argmin(np.abs(z - zz)))][c - h:c + h + 1, c - h:c + h + 1]
            cells.append(img(ctx.tile("%s-z%+.1f" % (tag, zz), pl, cmap="magma", vmin=0, min_px=96)))
        out.append(cells)
    return table(["preset", "pupil phase", "rms (waves)", "x-z (log)", "z = -0.6 um", "z = 0", "z = +0.6 um"], out,
                 cls="isc-cmp isc-small") + \
        "\nEvery Zernike preset (`Objective.ZernikePreset`, cli `psf-zernike-preset`), " \
        "default label (678 nm, NA 1.4). Pupil phase: the Zernike wavefront over the pupil (blue to red, each row " \
        "its own scale; its rms beside it). x-z: 2 um wide, 2.4 um deep, four decades. x-y planes: 2 um across, " \
        "linear, each its own scale. The astigmatic and extended-range presets stretch the spot one way above " \
        "focus and the other way below (z encoding).\n"


@register("psf-pupil", "optics", timed=True)
def psf_pupil(ctx):
    base = {"psf-z-range-um": 7, "psf-z-step-um": 0.5}
    runs = {}
    for name, n in (("64 (webSMLM)", 64), ("automatic (default)", 0), ("512 (reference)", 512)):
        planes, cams, pupil, meta, r = psf(ctx, dict(base, **{"psf-pupil-samples": n, "disk-cache": 1}), "M%d" % n,
                                           fresh=True)
        runs[name] = (planes, cams, meta, r.wall)
    ref_cams = runs["512 (reference)"][1]
    z = zs(runs["512 (reference)"][2])
    cols, sections, errs, errs_far, times = [], [], [], [], []
    for name, (planes, cams, meta, wall) in runs.items():
        cols.append("%s<br>%d samples" % (name, meta["pupil_samples"]))
        sections.append(img(log_tile(ctx, name.split()[0] + "-z3", cams[int(np.argmin(np.abs(z - 3.0)))], 6)))
        d = np.abs(cams - ref_cams)
        errs.append(sig(float(d.max()), 2))
        errs_far.append(sig(float(d[np.abs(z) >= 2].max()), 2))
        times.append(secs(wall))
    return table([""] + cols,
                 [["an emitter 3 um out of focus on the camera (log, 6 decades)"] + sections,
                  ["largest error per camera pixel (share of the emitter's photons)"] + errs,
                  ["... beyond 2 um defocus"] + errs_far,
                  ["cli run time (the kernel)"] + times]) + \
        "\nThe pupil grid of the chirp-Z evaluation (`Renderer.PsfPupilSamples`, cli `psf-pupil-samples`) on the " \
        "default +/-7 um kernel (planes every 0.5 um here). A sampled pupil makes the PSF repeat every " \
        "(M-4)&lambda;/(2NA): with 64 samples that period is shorter than the window and defocused light folds " \
        "back in (the square pattern around the defocused spot). Errors: the camera image of a 1-photon emitter " \
        "against the 512-sample reference, over all planes. Times: the kernel computed in a fresh process, on the " \
        "build machine (%d threads).\n" % cores()


@register("psf-halo", "optics", timed=True)
def psf_halo(ctx):
    levels = [(q["name"], q["psf-halo-cut"]) for q in presets(ctx)["quality"]]
    tiles = {0.0: [], 2.0: []}
    kept, missing, missing_def = [], [], []
    for name, cut in levels:
        for zz in (0.0, 2.0):
            a, meta, _ = splat(ctx, {"psf-halo-cut": cut}, "%s-z%g" % (name, zz), 0.0, 0.0, zz)
            tiles[zz].append(img(log_tile(ctx, "%s-z%g" % (name, zz), a, 7, ref=1.0)))
            if zz == 0.0:
                kept.append("%.0f %%" % (100 * meta["kept_pixels"] / meta["square_pixels"]))
                missing.append(share(1 - meta["sum"]))
            else:
                missing_def.append(share(1 - meta["sum"]))
    movies, times = {}, []
    for name, cut in levels:
        r = ctx.cli(dict(SPOT, size=128, frames=50, **{"psf-halo-cut": cut}), tag=name + "-movie", photons_out=True)
        movies[name] = stack(r["photons_out"]).mean(0)
        t = r.time("photons_out")
        times.append(secs(t[1] - t[0]))
    ref = movies[levels[-1][0]]
    dmax = [sig(float(np.abs(movies[n] - ref).max()), 2) for n, _ in levels]
    drel = [sig(100 * float(np.abs(movies[n] - ref).sum() / ref.sum()), 2) for n, _ in levels]
    return table([""] + ["%s<br>cut %s" % (n, sig(c, 1) if c else "0") for n, c in levels],
                 [["one blink in focus (log, 7 decades)"] + tiles[0.0],
                  ["one blink 2 um out of focus"] + tiles[2.0],
                  ["pixels drawn, in focus (of the square)"] + kept,
                  ["photons left out, in focus"] + missing,
                  ["... 2 um out of focus"] + missing_def,
                  ["50-frame mean: largest difference to Exhaustive (photons/px)"] + dmax,
                  ["... summed absolute difference / total photons (%)"] + drel,
                  ["render time, 50 frames of 128 px"] + times]) + \
        "\nThe halo cut of blinking emitters (`Renderer.Quality` sets `Renderer.PsfHaloCut`; cli `psf-halo-cut`): " \
        "pixels that would get less than the cut's share of the photons are left out, and nothing is renormalized. " \
        "Images: a 1-photon blink over the whole +/-7 um kernel square (default label), black = left out. The " \
        "movie rows compare the mean photon image (before the camera) of the default DNA-PAINT movie with the " \
        "Exhaustive one. Times on the build machine (%d threads).\n" % cores()


@register("psf-interp", "optics", timed=True)
def psf_interp(ctx):
    small = {"psf-kernel-half-width-nm": 1500, "psf-z-range-um": 0.4, "psf-halo-cut": 0}
    modes = ["Nearest", "Linear", "Cubic", "Fft"]
    shifts = np.round(np.linspace(0, 0.5, 6), 3)
    show = 0.3
    images, bias = {}, {m: [] for m in modes}
    for m in modes:
        for dx in shifts:
            a, meta, _ = splat(ctx, dict(small, **{"psf-interp": m}), "%s-%g" % (m, dx), dx, 0.0, 0.0)
            w = centre(a, 4)
            xs = np.arange(-4, 5)
            bias[m].append(float((w.sum(0) * xs).sum() / w.sum()) * meta.get("pixel_nm", 100))
            if abs(dx - show) < 1e-9:
                images[m] = w
    ref = images["Fft"]
    bias = {m: [b - f for b, f in zip(bias[m], bias["Fft"])] for m in modes}
    times = []
    for m in modes:
        size, frames = (16, 1) if m == "Fft" else (64, 20)
        r = ctx.cli(dict(SPOT, size=size, frames=frames, **{"psf-interp": m}), tag=m + "-movie", out=True)
        t = r.time("out")
        n = int(re.search(r"(\d+) blinks", r.stdout).group(1))
        times.append("%s<br><small>%d blinks</small>" % (sig(1000 * (t[1] - t[0]) / max(n, 1), 2), n))
    scale = max(float(np.abs(images[m] - ref).max()) for m in modes) or 1e-9

    def draw(fig, st):
        ax = fig.subplots()
        for k, m in enumerate(modes):
            ax.plot(shifts, bias[m], "o-", color=COLORS[k], ms=3, label=m)
        ax.axhline(0, color=st["grid"], lw=0.8)
        ax.set_xlabel("emitter offset from the pixel centre (px)")
        ax.set_ylabel("centroid - Fft's (nm)")
        ax.legend(ncol=4)

    curve = ctx.plot("bias", draw, h=1.9, alt="Centroid bias per placement method")
    return table([""] + modes,
                 [["blink at +%g px (9 x 9 px)" % show] +
                  [img(ctx.tile(m, images[m], vmin=0, vmax=float(ref.max()))) for m in modes],
                  ["difference to Fft (&plusmn;%s of its photons)" % sig(scale, 2)] +
                  [img(ctx.diff_tile(m + "-d", images[m] - ref, scale)) for m in modes],
                  ["largest difference (share of the photons)"] +
                  [sig(float(np.abs(images[m] - ref).max()), 2) for m in modes],
                  ["render time per blink (ms) in a movie"] + times]) + "\n" + \
        figure(curve, "How each placement (`Renderer.PsfInterp`, cli `psf-interp`) samples the oversampled kernel at "
                      "the emitter's sub-pixel position (default label, in focus, halo cut off): Nearest snaps to "
                      "the kernel's 1/6-pixel grid, Linear and Cubic interpolate, Fft shifts by a Fourier phase "
                      "(exact for the band-limited kernel, slow). The curve: each method's 9 x 9 px centroid minus "
                      "Fft's at the same offset (100 nm pixels). Times: the default DNA-PAINT movie (Fft: one 16 px "
                      "frame), on the build machine (%d threads)." % cores())


@register("psf-oversampling", "optics", timed=True)
def psf_oversampling(ctx):
    small = {"psf-kernel-half-width-nm": 2000, "psf-z-range-um": 0.4, "psf-halo-cut": 0}
    levels = [2, 4, 6, 8, 12, 16]
    imgs = {o: splat(ctx, dict(small, **{"psf-oversampling": o}), "os%d" % o, 0.37, -0.21, 0.0)[0] for o in levels}
    ref = imgs[16]
    # Compute times of the default label's default kernel: 4, 6 and 8 from the build's kernel pools (each computed
    # alone before the figures), 2 here in a fresh process.
    pt = pool_times(ctx.cache_root)
    times = {o: pt.get(name) for o, name in ((4, "os4"), (6, "default"), (8, "os8")) if pt.get(name)}
    r = ctx.cli({"psf-oversampling": 2, "prepare": 1, "disk-cache": 1}, tag="prep2", fresh=True)
    m = re.search(r"PSF kernel ([\d.]+) s", r.stdout)
    times[2] = float(m.group(1)) if m else float("nan")
    who = {q["psf-oversampling"]: q["name"] for q in presets(ctx)["quality"]}
    return table(["oversampling"] + ["%d%s" % (o, "<br>(%s)" % who[o] if o in who else "") for o in levels],
                 [["largest error per camera pixel vs 16 (share of the photons)"] +
                  [sig(float(np.abs(imgs[o] - ref).max()), 2) if o != 16 else "reference" for o in levels],
                  ["default kernel (+/-7 um, 71 planes): compute time"] +
                  [secs(times[o]) if o in times else "-" for o in levels]]) + \
        "\nThe kernel's samples per camera pixel (`Renderer.PsfOversampling`, cli `psf-oversampling`): one in-focus " \
        "blink at (+0.37, -0.21) px, cubic placement, halo cut off, against oversampling 16. The time is the " \
        "default label's default kernel computed by `--prepare 1` with nothing else running, on the build machine " \
        "(%d threads).\n" % cores()


# ---- WideField --------------------------------------------------------------------------------------

@register("wf-pipeline", "optics")
def wf_pipeline(ctx):
    wf = typical("WideField")
    r = ctx.cli(dict(SPOT, size=96, frames=1, **wf, **{"density-z": [0, 4, 8]}), tag="wf", density_out=True,
                photons_out=True, out=True)
    dens = stack(r["density_out"])
    dmeta = load(r["density_out"] + ".json")
    ph = stack(r["photons_out"])[0]
    adu = stack(r["out"])[0]
    planes, cams, pupil, meta, _ = psf(ctx, dict(wf, **{"psf-kernel-half-width-nm": 3000}), "psf")
    z = zs(meta)
    dz = (dmeta["z_max"] - dmeta["z_min"]) / dmeta["nz"]
    picks = [0, 2, 4, 6]
    vmax = float(dens.max()) or 1.0
    dens_tiles = [img(ctx.tile("dens%d" % k, dens[k], cmap="viridis", vmin=0, vmax=vmax)) for k in picks]
    heads = ["dyes at %.1f-%.1f um" % (dmeta["z_min"] + k * dz, dmeta["z_min"] + (k + 1) * dz) for k in picks]
    psf_tiles = []
    for zz in (0.0, 1.0, 2.0):
        pl = centre(cams[int(np.argmin(np.abs(z - zz)))], 15)
        psf_tiles.append(img(ctx.tile("psf%g" % zz, pl, cmap="magma", vmin=0)))
    lo, hi = robust_range(adu, 0.5, 99.9)
    return table(heads, [dens_tiles]) + \
        table(["PSF in focus", "1 um out", "2 um out", "mean-field photons", "camera frame"],
              [psf_tiles + [img(ctx.tile("photons", ph, vmin=0, vmax=robust_range(ph, hi=99.9)[1])),
                            img(ctx.tile("frame", adu, cmap="gray", vmin=lo, vmax=hi))]]) + \
        "\nThe WideField path for mEGFP (70 %% labelled, 488 nm): the labelled dyes counted on z planes (top: %d of " \
        "the %d 0.5 um planes the cli wrote, one scale; the renderer itself bins on 25 nm planes), each plane " \
        "convolved with the PSF at its defocus (a 1-photon emitter on the camera, 3.1 um across, each its own " \
        "scale), summed into the photon image, then the camera. 9.6 um field, focus 0.5 um above the coverslip, " \
        "one 50 ms frame.\n" % (len(picks), dmeta["nz"])


def image_shift(a, ref):
    """The sub-pixel shift (x, y in px) that best explains a - ref, to first order: a - ref = -dx d/dx ref - dy d/dy ref
    (least squares; noise in a that does not follow the gradient does not bias it)."""
    gy, gx = np.gradient(ref)
    (sx, sy), *_ = np.linalg.lstsq(np.stack([-gx.ravel(), -gy.ravel()], 1), (a - ref).ravel(), rcond=None)
    return float(sx), float(sy)


@register("wf-grid", "optics", timed=True)
def wf_grid(ctx):
    base = dict(SPOT, size=64, frames=1, **typical("WideField"), **{"mt-label-pct": 10})
    mean_field = {"mean-field-density-per-um2": 0}
    cases = [("upscale 1, 25 nm planes (default)", 1, 25), ("upscale 2", 2, 25), ("upscale 3", 3, 25),
             ("100 nm planes", 1, 100), ("50 nm planes", 1, 50), ("12.5 nm planes", 1, 12.5),
             ("upscale 3, 12.5 nm planes (reference)", 3, 12.5)]
    ctx.cli(dict(base, prepare=1), tag="warm")   # the kernel, so each setup below is the grid's own
    out = {}
    for name, up, plane in cases:
        r = ctx.cli(dict(base, **mean_field, **{"wf-upscale": up, "wf-plane-nm": plane}), tag="u%d-p%g" % (up, plane),
                    photons_out=True)
        out[name] = (stack(r["photons_out"])[0], r.time("photons_out"))
    # The same dyes one by one at their true positions: one realization of which dyes have bleached (each has its own
    # photon budget), so not a reference for the grid error, but for where the image sits.
    r = ctx.cli(dict(base, **{"mean-field-density-per-um2": 1e12, "mean-field-max-emitters": 1e12}), tag="perdye",
                photons_out=True)
    per_dye = stack(r["photons_out"])[0]
    ref_name = cases[-1][0]
    ref = out[ref_name][0]
    scale = max(float(np.abs(out[n][0] - ref).max()) for n, _, _ in cases[:-1]) or 1e-9
    body, shifts = [], {}
    for name, up, plane in cases:
        a, t = out[name]
        d = a - ref
        sx, sy = image_shift(a, per_dye)
        shifts[name] = (100 * sx, 100 * sy)
        is_ref = name == ref_name
        body.append([name,
                     img(ctx.tile("ref", ref, vmin=0, vmax=robust_range(ref, hi=99.9)[1])) if is_ref else
                     img(ctx.diff_tile("u%d-p%g" % (up, plane), d, scale)),
                     "" if is_ref else sig(100 * float(np.sqrt(np.mean(d ** 2)) / ref.mean()), 2),
                     "" if is_ref else sig(float(np.abs(d).max() / np.sqrt(ref.max())), 2),
                     "%+.0f, %+.0f" % shifts[name], secs(t[0]), secs(t[1] - t[0])])
    off = [n for n, (x, y) in shifts.items() if np.hypot(x, y) > 4]
    note = ""
    if off:
        note = " The mean field puts the image off the dyes' true positions by up to %.0f nm for %s (the last " \
               "column, fitted against the dyes rendered one by one), which also counts in the difference to the " \
               "reference." % (max(np.hypot(*shifts[n]) for n in off), "upscale %s" % "/".join(
                   sorted({str(up) for n, up, _ in cases if n in off})))
    return table(["mean-field grid", "difference to the reference (&plusmn;%s photons)" % sig(scale, 2),
                  "rms (% of the mean)", "largest / shot noise", "image shift x, y vs per dye (nm)", "setup", "frame"],
                 body) + \
        "\nThe mean-field grid: cells per camera pixel (`Renderer.WideFieldUpscaling`, cli `wf-upscale`; it has to " \
        "divide the PSF oversampling, so 4 becomes 3 at the default 6) and dye plane thickness " \
        "(`Renderer.WideFieldZPlaneNm`, `wf-plane-nm`), against the finest grid; mEGFP WideField, 10 %% labelled, " \
        "64 px, one 50 ms frame, photons before the camera. 'Largest / shot noise': the largest difference over the " \
        "square root of the brightest pixel (below 1 it hides in one frame's noise).%s Setup: the scene (the PSF " \
        "kernel was loaded before). Times on the build machine (%d threads).\n" % (note, cores())


@register("wf-defocus", "optics")
def wf_defocus(ctx):
    foci = [0.25, 1.0, 2.0, 3.0]
    tiles = []
    for zz in foci:
        r = ctx.cli(dict(SPOT, size=96, frames=1, z=zz, **typical("WideField")), tag="z%g" % zz, out=True)
        a = stack(r["out"])[0]
        lo, hi = robust_range(a, 0.5, 99.9)
        tiles.append(img(ctx.tile("z%g" % zz, a, cmap="gray", vmin=lo, vmax=hi)))
    return table(["focus %g um" % z for z in foci], [tiles]) + \
        "\nmEGFP WideField frames at four focal heights above the coverslip (the `ZStage`, cli `z`): the " \
        "microtubules near the coverslip blur as the focus rises into the cell, and the higher ones (over the " \
        "nucleus) sharpen. Each image its own contrast.\n"


# ---- BrightField ------------------------------------------------------------------------------------

BF = {"modality": "BrightField"}


@register("bf-scene", "optics")
def bf_scene(ctx):
    r = ctx.cli(dict(SPOT, size=96, frames=1, z=-1, **BF), tag="bf", bf_screens_out=True, photons_out=True,
                setup_json=True)
    meta = load(r["bf_screens_out"] + ".json")
    setup = load(r["setup_json"])
    phase = stack(r["bf_screens_out"] + ".phase.tif")
    ph = stack(r["photons_out"])[0]
    n = len(phase)
    picks = sorted(set(np.linspace(0, n - 1, min(n, 4)).round().astype(int)))
    vmax = float(np.abs(phase).max()) or 1e-9
    t = ph / float(np.median(ph))
    return table(["slice %d of %d" % (k + 1, n) for k in picks] + ["image, focus 1 um below"],
                 [[img(ctx.tile("phase%d" % k, phase[k], cmap="viridis", vmin=0, vmax=vmax)) for k in picks] +
                  [img(ctx.tile("image", t, cmap="gray", vmin=robust_range(t)[0], vmax=robust_range(t, hi=99.5)[1]))]]) + \
        "\nWhat BrightField sees: the phase delay k<sub>0</sub>&Delta;z&Delta;n of each %s um slice of the cells' " \
        "refractive index (cytoplasm, nucleus, microtubules), from the core's optical volume, on the optical grid " \
        "with its margin (one scale, at most %s rad). The lamp's light (%g nm) is propagated through the slices " \
        "(multislice, %d condenser source points) and refocused; a weak phase object shows contrast mainly out of " \
        "focus. Default quality 3.\n" % (sig(setup["brightfield"]["slice_um"], 2), sig(vmax, 2),
                                          meta["wavelength_nm"], meta["sources"])


@register("bf-quality", "optics", timed=True)
def bf_quality(ctx):
    base = dict(SPOT, size=96, frames=1, z=-1, **BF)
    levels = [("1", {"bf-quality": 1}), ("2", {"bf-quality": 2}), ("3 (default)", {"bf-quality": 3}),
              ("4", {"bf-quality": 4}),
              ("reference", {"bf-quality": 4, "bf-sources": 96, "bf-sub": 4, "bf-slice-um": 0.125, "bf-margin-um": 6})]
    out = {}
    for name, opts in levels:
        r = ctx.cli(dict(base, **opts), tag="q" + name.split()[0], photons_out=True)
        out[name] = (stack(r["photons_out"])[0], r.time("photons_out"))
    ref = out["reference"][0]
    flux = float(np.median(ref))
    c0, c1 = robust_range(ref, 0.5, 99.5)
    scale = max(float(np.abs(out[n][0] - ref).max()) for n, _ in levels[:-1]) / flux
    row_img, row_d, row_rms, row_max, row_setup, row_frame = [], [], [], [], [], []
    for name, _ in levels:
        a, t = out[name]
        tag = name.split()[0]
        row_img.append(img(ctx.tile("q" + tag, a / flux, cmap="gray", vmin=c0 / flux, vmax=c1 / flux)))
        is_ref = name == "reference"
        row_d.append("" if is_ref else img(ctx.diff_tile("d" + tag, (a - ref) / flux, scale)))
        row_rms.append("" if is_ref else sig(100 * float(np.sqrt(np.mean((a - ref) ** 2))) / (c1 - c0), 2))
        row_max.append("" if is_ref else sig(float(np.abs(a - ref).max() / np.sqrt(flux)), 2))
        row_setup.append(secs(t[0]))
        row_frame.append(secs(t[1] - t[0]))
    return table(["BrightField quality"] + [n for n, _ in levels],
                 [["image (1 = the lamp)"] + row_img,
                  ["difference to the reference (&plusmn;%s of the lamp)" % sig(scale, 2)] + row_d,
                  ["rms difference (% of the image's contrast)"] + row_rms,
                  ["largest difference / shot noise of one frame"] + row_max,
                  ["setup"] + row_setup, ["frame"] + row_frame]) + \
        "\n`Renderer.BrightFieldQuality` (cli `bf-quality`), focal plane 1 um below the coverslip (z = -1, where the " \
        "cells show contrast), 96 px, against a reference beyond level 4 (96 source points, 4 geometry samples per " \
        "cell side, 0.125 um slices, 6 um margin). Photons before the camera as a share of the lamp; one frame's " \
        "shot noise at the default 80000 photons/px/s and 50 ms. Times on the build machine (%d threads).\n" % cores()


@register("bf-defocus", "optics")
def bf_defocus(ctx):
    foci = [-2, -1, 0, 1, 2]
    base = dict(SPOT, size=96, frames=1, **BF)
    focus = []
    for zz in foci:
        a = stack(ctx.cli(dict(base, z=zz), tag="z%g" % zz, photons_out=True)["photons_out"])[0]
        focus.append(a / np.median(a))
    lo = min(robust_range(a)[0] for a in focus)
    hi = max(robust_range(a, hi=99.5)[1] for a in focus)
    row1 = [img(ctx.tile("z%g" % zz, a, cmap="gray", vmin=lo, vmax=hi)) for zz, a in zip(foci, focus)]
    nas = [0.0, 0.4, 0.9]
    row2 = []
    for na in nas:
        a = stack(ctx.cli(dict(base, z=-1, **{"bf-condenser-na": na}), tag="na%g" % na, photons_out=True)
                  ["photons_out"])[0]
        row2.append(img(ctx.tile("na%g" % na, a / np.median(a), cmap="gray", vmin=lo, vmax=hi)))
    return table(["focus %+g um" % z for z in foci], [row1]) + \
        table(["condenser NA %g%s" % (na, " (default)" if na == 0.4 else "") for na in nas], [row2]) + \
        "\nBrightField through focus (photons before the camera, 1 = the lamp, one grey scale for all): the cells " \
        "are weak phase objects, faint in focus, with opposite contrast below and above it. Bottom, at z = -1 um: " \
        "a smaller condenser aperture (more coherent light, `TransmittedLamp.CondenserNA`) gives more contrast and "\
        "fringes, a larger one less.\n"
