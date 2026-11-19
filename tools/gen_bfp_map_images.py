# SPDX-License-Identifier: BSD-3-Clause
"""The example BFP map images (web/bfp_maps/*.png, the viewer's BFP maps Examples; spec/ALGORITHM.md "BFP light maps").

An image spans the pupil's square: u in [-umax, umax] on both axes (umax = NA / n_s, 1.4 / 1.337 at the defaults; u = 1 is
the critical angle), row 0 at +uy; its pixel values are the map's (here the intensity I, relative: 0..255). A ring at
the critical angle therefore sits at 1 / umax of the half width at the default NA; another NA moves it.

    python tools/gen_bfp_map_images.py [--check]
"""
import io
import os
import sys

import numpy as np
from PIL import Image

N = 256
UMAX = 1.4 / 1.337   # the default objective (NA 1.4) and medium (1.337)
OUT = os.path.join(os.path.dirname(__file__), "..", "web", "bfp_maps")


def grid():
    c = -UMAX + (np.arange(N) + 0.5) * 2 * UMAX / N
    ux, uy = np.meshgrid(c, c[::-1])   # row 0 = +uy
    return ux, uy


def spots(points, radius):
    ux, uy = grid()
    a = np.zeros((N, N))
    for x, y in points:
        a = np.maximum(a, np.exp(-2 * ((ux - x) ** 2 + (uy - y) ** 2) / radius ** 2))
    return a


def images():
    ux, uy = grid()
    r = np.hypot(ux, uy)
    hexagon = [(0.8 * np.cos(k * np.pi / 3), 0.8 * np.sin(k * np.pi / 3)) for k in range(6)]
    return {
        # six coherent spots on a hexagon at u = 0.8: a hexagonal lattice in the focal plane
        "hex_lattice.png": spots(hexagon, 0.02),
        # four spots at u = 0.6 on the axes: a square lattice
        "square_lattice.png": spots([(0.6, 0), (-0.6, 0), (0, 0.6), (0, -0.6)], 0.02),
        # a centre spot and two at u = +-0.8 (3D SIM's three beams): fringes laterally and along z
        "three_beam_sim.png": spots([(0, 0), (0.8, 0), (-0.8, 0)], 0.02),
        # a line across the pupil along uy (a cylindrical lens): light focused to a line along x at the focal plane
        "line_focus.png": np.exp(-2 * ux ** 2 / 0.02 ** 2) * (r < 0.95 * UMAX),
        # a thin annulus just past the critical angle, all round at once: TIRF from every direction (incoherent)
        "annulus_tirf.png": np.exp(-2 * (r - 1.02) ** 2 / 0.01 ** 2),
        # the pupil's left half (incoherent): oblique light from one side
        "half_pupil.png": ((ux < 0) & (r < UMAX)).astype(float),
    }


def png(a):
    b = io.BytesIO()
    Image.fromarray(np.round(np.clip(a, 0, 1) * 255).astype(np.uint8), mode="L").save(b, "PNG", optimize=True)
    return b.getvalue()


def main():
    check = "--check" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    bad = []
    for name, a in images().items():
        p = os.path.join(OUT, name)
        data = png(a)
        if check:
            old = np.asarray(Image.open(p)) if os.path.exists(p) else None
            if old is None or not np.array_equal(old, np.asarray(Image.open(io.BytesIO(data)))):
                bad.append(name)
        else:
            with open(p, "wb") as f:
                f.write(data)
            print("wrote", os.path.relpath(p), len(data), "bytes")
    if bad:
        sys.exit("out of date: %s (python tools/gen_bfp_map_images.py)" % ", ".join(bad))
    if check:
        print("bfp map images: up to date")


if __name__ == "__main__":
    main()
