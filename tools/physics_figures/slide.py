"""Fluorescent slide page (docs/samples/slide.md): the slide's photons under the Gaussian beam, fresh and after it has
bleached its centre (the light history pixel by pixel, here start-sec under the movie's own light)."""
import numpy as np

from .common import COLORS, figure, img, register, stack, table

TIMES = [0, 1800, 3600]   # s of light before the frame


@register("slide-bleach", "slide")
def slide_bleach(ctx):
    # The default slide (5 um, the Mix at 10 uM, a 900 s half-time at 1 kW/cm^2) with its 561 nm line at 0.5 kW/cm^2
    # (its preset's 0.02 bleaches it over hours), the default Gaussian beam (FWHM = the field of view), epi.
    base = {"specimen": "Slide", "size": 128, "frames": 1, "laser-561": 0.5, "illum-bfp": 0}
    arrays = []
    for t in TIMES:
        r = ctx.cli(dict(base, **{"start-sec": t}), tag="t%d" % t, photons_out=True)
        arrays.append(stack(r["photons_out"])[0].astype(np.float64))
    hi = max(a.max() for a in arrays)
    tiles = [ctx.tile("t%d" % t, a, cmap="magma", vmin=0, vmax=hi) for t, a in zip(TIMES, arrays)]
    mid = arrays[0].shape[0] // 2
    x = (np.arange(arrays[0].shape[1]) - (arrays[0].shape[1] - 1) / 2) * 0.1

    def draw(fig, st):
        ax = fig.subplots()
        for k, (t, a) in enumerate(zip(TIMES, arrays)):
            ax.plot(x, a[mid], color=COLORS[k], lw=1.2, label="after %d min" % (t // 60) if t else "fresh")
        ax.set_xlabel("Position across the field (µm)")
        ax.set_ylabel("Photons per pixel per frame")
        ax.set_ylim(0, None)
        ax.legend(frameon=False)

    body = table(["Fresh", "After 30 min", "After 60 min"], [[img(t) for t in tiles]], label_col=False) + "\n" + \
        ctx.plot("", draw, h=2.6, alt="Profiles through the slide's centre, fresh and bleached")
    return figure(body,
                  "The fluorescent slide at its defaults (5 µm of the TetraSpeck mix at 10 µM each, its 900 s half-time "
                  "at 1 kW/cm²; the 561 nm channel, here at 0.5 kW/cm²), under the default Gaussian beam (FWHM = the "
                  "128 px field of 100 nm pixels), epi: one 50 ms frame's photons before the camera, one colour scale, "
                  "fresh and after 30 and 60 minutes of this light. The fresh image is the beam's profile, softened by "
                  "the layer's out-of-focus light; the centre, lit most, bleaches first, so the profile flattens. The "
                  "561 nm line excites the orange dye only (the mix's others are not detected through 600/37).")
