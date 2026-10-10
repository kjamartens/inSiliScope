"""Renderer.Quality: the Fast / Realistic / Exhaustive bundle on SMLM, WideField and BrightField side by side
(optics page)."""
import numpy as np

from .common import BUILD_INFO, SPOT, img, load, register, robust_range, secs, sig, stack, table
from .light import typical

# (row label, movie options, image: the mean of the frames or the first one)
MODALITIES = [
    ("SMLM: mean of 30 frames", dict(SPOT, size=96, frames=30), "mean"),
    ("WideField: one frame", dict(SPOT, size=64, frames=10, **typical("WideField")), "first"),
    ("BrightField: focus 1 µm below", dict(SPOT, size=96, frames=10, z=-1, modality="BrightField"), "first"),
]


@register("quality-bundle", "optics", timed=True)
def quality_bundle(ctx):
    pr = load(ctx.cli({}, tag="presets", presets_json=True)["presets_json"])["quality"]
    names = [q["name"] for q in pr]
    knobs = {q["name"]: {k: q[k] for k in ("bf-quality", "psf-oversampling", "wf-upscale", "psf-halo-cut")} for q in pr}
    body = []
    for mod, base, which in MODALITIES:
        tag = mod.split(":")[0]
        out = {}
        for q in names:
            r = ctx.cli(dict(base, **knobs[q]), tag="%s-%s" % (tag, q), photons_out=True)
            ph = stack(r["photons_out"])
            out[q] = (ph.mean(0) if which == "mean" else ph[0], r.time("photons_out"))
        ref = out[names[-1]][0]
        lo, hi = robust_range(ref, 0.5, 99.8)
        scale = max(float(np.abs(out[q][0] - ref).max()) for q in names[:-1]) or 1e-9
        cmap = "gray" if "Bright" in mod else "magma"
        body.append(["**%s**" % mod] +
                    [img(ctx.tile("%s-%s" % (tag, q), out[q][0], cmap=cmap, vmin=lo, vmax=hi)) for q in names])
        body.append(["Difference to Exhaustive (&plusmn;%s photons)" % sig(scale, 2)] +
                    [img(ctx.diff_tile("%s-%s-d" % (tag, q), out[q][0] - ref, scale)) if q != names[-1] else ""
                     for q in names])
        cells = []
        for q in names:
            a, t = out[q]
            rms = "-" if q == names[-1] else "%s %%" % sig(100 * float(np.sqrt(np.mean((a - ref) ** 2))) / (hi - lo), 2)
            cells.append("%s<br>%s" % (rms, secs((t[1] - t[0]) / base["frames"])))
        body.append(["rms difference (% of the range)<br>Render time per frame"] + cells)
    head = ["`Renderer.Quality`"] + ["%s<br><small>bf %g, oversampling %g, wf %g, halo %s</small>" %
                                     (q, knobs[q]["bf-quality"], knobs[q]["psf-oversampling"], knobs[q]["wf-upscale"],
                                      sig(knobs[q]["psf-halo-cut"], 1) if knobs[q]["psf-halo-cut"] else "0")
                                     for q in names]
    return table(head, body) + \
        "\nThe one Micro-Manager knob for speed against precision, `Renderer.Quality`, sets four engine options " \
        "(the cli's preset table, `Simulation/RenderPresets.h`): the BrightField quality, the PSF oversampling, " \
        "the mean-field grid and the blink halo cut. Photon images before the camera of the default label (SMLM), " \
        "mEGFP (WideField) and the lamp (BrightField), each against Exhaustive. Times on the build machine (%d " \
        "threads), per frame (averaged over 30 SMLM or 10 WideField and BrightField frames), without the setup; the " \
        "PSF kernel each oversampling needs costs once per session (computed or read from the disk cache), see the " \
        "oversampling table above.\n" % BUILD_INFO.get("cores", 0)
