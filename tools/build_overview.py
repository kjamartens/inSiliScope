#!/usr/bin/env python3
"""The gallery's and Home page's 2x2 overview of one spot of the cell field.

    (a) overview_map.png         many cells around the spot, the camera's field of view boxed
    (b) overview_structures.png  the structures in that box, in the viewer's colours: cell, nucleus, microtubules, pores
    (c) overview_wf.webp         a widefield movie of the box
    (d) overview_sr.webp         an SMLM movie of the box

Driven by the "overview" block of gallery/manifest.json; build_gallery.py calls build(). The geometry comes from
`insiliscope_cli --geometry-json` (the same world the movies render), drawn here with Pillow. Image row 0 is the
smallest world y in every panel, as in the camera images.

usage: build_overview.py --cli <insiliscope_cli> --out <dir> [--manifest gallery/manifest.json]
       build_overview.py --cli <insiliscope_cli> --suggest     # print a spot near the origin to put in the manifest
Needs: numpy, pillow, tifffile.
"""
import argparse, json, math, os, subprocess, sys, tempfile
import numpy as np
import tifffile
from PIL import Image, ImageDraw, ImageFont

PANEL = 512           # px, every panel
SS = 2                # supersampling for the drawn panels
BG = (17, 17, 17)

def label_font(px):
    """A TrueType font with a micro sign (Pillow's built-in one has none); None if none is found."""
    for name in ("DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "arial.ttf",
                 "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            pass
    return None


def cli_geometry(cli, opts, size_um, detail):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "g.json")
        args = [cli, "--geometry-json", path, "--geometry-um", str(size_um), "--geometry-detail", "1" if detail else "0"]
        for k, v in opts.items():
            args += ["--" + k, str(v)]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("geometry failed: " + r.stderr[-400:])
        return json.load(open(path, encoding="utf-8"))


class Canvas:
    """World um -> supersampled pixels of a square panel."""

    def __init__(self, geo, bg=BG):
        self.x0, self.y0 = geo["x0"], geo["y0"]
        self.s = PANEL * SS / (geo["x1"] - geo["x0"])
        self.img = Image.new("RGB", (PANEL * SS, PANEL * SS), bg)
        self.draw = ImageDraw.Draw(self.img, "RGBA")

    def p(self, x, y):
        return ((x - self.x0) * self.s, (y - self.y0) * self.s)

    def poly(self, pts):
        return [self.p(x, y) for x, y in pts]

    def scale_bar(self, um, label):
        w = um * self.s
        x, y = 24 * SS, PANEL * SS - 30 * SS
        self.draw.rectangle([x, y, x + w, y + 3 * SS], fill=(240, 240, 240))
        font = label_font(16 * SS)
        if font is None:
            label = label.replace("µm", "um")
        self.draw.text((x, y - 21 * SS), label, fill=(240, 240, 240), font=font, font_size=16 * SS)

    def save(self, path):
        self.img.resize((PANEL, PANEL), Image.LANCZOS).save(path, optimize=True)


def nucleus_poly(n, k=48):
    c, s = math.cos(n["rot"]), math.sin(n["rot"])
    pts = []
    for i in range(k):
        t = 2 * math.pi * i / k
        lx, ly = math.cos(t) * n["long"] / 2, math.sin(t) * n["short"] / 2
        pts.append((n["x"] + lx * c - ly * s, n["y"] + lx * s + ly * c))
    return pts


# ---- (b) structure layers: one function each, drawn in this order -------------
# A new structure (NPCs, DNA, ...) is one more function here, fed by more geometry from ScopeGeometryJson.
# The colours are the viewer's default look, "Dark fluorescence (depth)" (web/scene/core.js THEMES.fluoZ; the scales
# LINE_MAPS in web/index.html, defaults ice / warm / greys): the cell in neutral greys on black, the microtubules and
# the nuclear pores coloured by their height from the coverslip to the top of their cell. Keep them in step.
VIEWER_BG = (0, 0, 0)
CELL_FILL = (70, 76, 88, int(0.3 * 255))
CELL_OUTLINE = (150, 160, 178, int(0.5 * 255))
ICE = [(0.25, 0.45, 0.95), (0.55, 0.78, 1), (0.95, 0.98, 1)]                       # microtubules
WARM = [(0.9, 0.22, 0.22), (1, 0.45, 0.15), (1, 0.7, 0.25), (1, 0.93, 0.62)]       # nuclear pores
GREYS = [(0.3, 0.32, 0.36), (0.65, 0.67, 0.7), (1, 1, 1)]                          # nucleus


def scale_rgb(stops, t):
    """A viewer colour scale at t in [0, 1] (stops evenly spaced, linear between), 0-255."""
    t = min(1.0, max(0.0, t)) * (len(stops) - 1)
    i = min(int(t), len(stops) - 2)
    f = t - i
    return tuple(int(round(255 * (a + (b - a) * f))) for a, b in zip(stops[i], stops[i + 1]))


def cell_top(c):
    """The top of a cell (its cytoplasm's highest point, um): the end of its height scale."""
    v = c.get("mesh", {}).get("v") or []
    return max((p[2] for p in v), default=0.0) or max((p[2] for mt in c["mts"] for p in mt), default=1.0) or 1.0


def layer_cell(cv, cells):
    for c in cells:
        cv.draw.polygon(cv.poly(c["outline"]), fill=CELL_FILL)


def layer_outline(cv, cells):
    for c in cells:
        pts = cv.poly(c["outline"])
        cv.draw.line(pts + pts[:1], fill=CELL_OUTLINE, width=SS)


def layer_microtubules(cv, cells):
    for c in cells:
        top = cell_top(c)
        for mt in c["mts"]:
            for a, b in zip(mt, mt[1:]):
                col = scale_rgb(ICE, (a[2] + b[2]) / 2 / top) + (230,)
                cv.draw.line([cv.p(a[0], a[1]), cv.p(b[0], b[1])], fill=col, width=SS)


def layer_nucleus(cv, cells):
    for c in cells:
        cv.draw.polygon(cv.poly(nucleus_poly(c["nucleus"])), fill=scale_rgb(GREYS, 0.5) + (int(0.22 * 255),))


def layer_pores(cv, cells, r=None):
    """The nuclear pores (geometry detail "npcs": centre, outward normal, e1): a dot per pore, coloured by its height
    as in the viewer."""
    r = r or max(1.5 * SS, 0.04 * cv.s)   # ~80 nm across, at least 3 px
    for c in cells:
        top = cell_top(c)
        for q in c.get("npcs", []):
            x, y = cv.p(q[0], q[1])
            cv.draw.ellipse([x - r, y - r, x + r, y + r], fill=scale_rgb(WARM, q[2] / top) + (235,))


STRUCTURE_LAYERS = [layer_cell, layer_nucleus, layer_microtubules, layer_pores, layer_outline]


def draw_map(geo, fov, path):
    cv = Canvas(geo)
    for c in geo["cells"]:
        pts = cv.poly(c["outline"])
        cv.draw.polygon(pts, fill=(45, 74, 107), outline=(90, 130, 170))
        cv.draw.polygon(cv.poly(nucleus_poly(c["nucleus"])), fill=(150, 110, 190))
    (bx0, by0), (bx1, by1) = cv.p(fov[0], fov[1]), cv.p(fov[2], fov[3])
    cv.draw.rectangle([bx0, by0, bx1, by1], outline=(255, 213, 74), width=3 * SS)
    cv.scale_bar(50, "50 µm")
    cv.save(path)


def draw_structures(geo, path):
    cv = Canvas(geo, VIEWER_BG)
    for layer in STRUCTURE_LAYERS:
        layer(cv, geo["cells"])
    cv.scale_bar(5, "5 µm")
    cv.save(path)


def movie(cli, opts, path_webp):
    with tempfile.TemporaryDirectory() as d:
        tif = os.path.join(d, "m.tif")
        args = [cli, "--out", tif]
        for k, v in opts.items():
            args += ["--" + k, str(v)]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("movie failed: " + r.stderr[-400:])
        stack = tifffile.imread(tif)
    if stack.ndim == 2:
        stack = stack[None]
    lo, hi = np.percentile(stack, 0.5), np.percentile(stack, 99.8)
    hi = max(hi, lo + 1)
    frames = []
    for f in stack:
        g = np.sqrt(np.clip((f.astype(np.float32) - lo) / (hi - lo), 0, 1))  # as the gallery GIFs
        frames.append(Image.fromarray((g * 255).astype(np.uint8), "L").convert("RGB"))
    frames[0].save(path_webp, save_all=True, append_images=frames[1:], duration=100, loop=0, quality=80, method=4)
    return " ".join(args[3:])


def build(cli, out, ov, seed=42):
    """Writes the four panels into out; returns metadata (options and CLI strings)."""
    os.makedirs(out, exist_ok=True)
    base = {"seed": seed, "x": ov["x"], "y": ov["y"], "size": ov["size"], "pixel-nm": ov["pixel-nm"]}
    fov_um = ov["size"] * ov["pixel-nm"] / 1000
    fov = (ov["x"] - fov_um / 2, ov["y"] - fov_um / 2, ov["x"] + fov_um / 2, ov["y"] + fov_um / 2)
    draw_map(cli_geometry(cli, base, ov["map-um"], False), fov, os.path.join(out, "overview_map.png"))
    draw_structures(cli_geometry(cli, base, fov_um, True), os.path.join(out, "overview_structures.png"))
    meta = {"overview": ov}
    for key in ("wf", "sr"):
        opts = dict(base)
        opts.update(ov[key])
        meta[key + "_cli"] = "insiliscope_cli --out %s.tif %s" % (key, movie(cli, opts, os.path.join(out, "overview_%s.webp" % key)))
    json.dump(meta, open(os.path.join(out, "overview.json"), "w"), indent=1)
    return meta


CAPTIONS = [("overview_map.png", "A field of synthetic cells; the box is the camera's view"),
            ("overview_structures.png", "The structures in the box: cell, nucleus, microtubules and nuclear pores"),
            ("overview_wf.webp", "Widefield movie of the box (Gibson-Lanni + Zernike PSF)"),
            ("overview_sr.webp", "SMLM movie of the box")]


def grid_html(prefix=""):
    figs = ['  <figure><img src="%s%s" alt="%s" loading="lazy"><figcaption>%s</figcaption></figure>' % (prefix, f, c, c)
            for f, c in CAPTIONS]
    return '<div class="isc-overview">\n' + "\n".join(figs) + "\n</div>"


def suggest(cli, seed):
    geo = cli_geometry(cli, {"seed": seed}, 160, False)
    c = min(geo["cells"], key=lambda c: math.hypot(c["x"], c["y"]))
    n = c["nucleus"]
    ox = sum(p[0] for p in c["outline"]) / len(c["outline"])
    oy = sum(p[1] for p in c["outline"]) / len(c["outline"])
    dx, dy = ox - n["x"], oy - n["y"]
    d = math.hypot(dx, dy) or 1.0
    # The nucleus edge at the centre of the box, cytoplasm on one side.
    x, y = n["x"] + dx / d * n["long"] / 2, n["y"] + dy / d * n["long"] / 2
    print(json.dumps({"x": round(x, 2), "y": round(y, 2)}))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--out")
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(__file__), "..", "gallery", "manifest.json"))
    ap.add_argument("--suggest", action="store_true")
    a = ap.parse_args()
    man = json.load(open(a.manifest, encoding="utf-8"))
    seed = man.get("defaults", {}).get("seed", 42)
    if a.suggest:
        suggest(a.cli, seed)
        return
    if not a.out:
        ap.error("--out is required")
    build(a.cli, a.out, man["overview"], seed)


if __name__ == "__main__":
    main()
