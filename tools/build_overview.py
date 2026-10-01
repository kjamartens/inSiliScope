#!/usr/bin/env python3
"""The gallery's and Home page's 2x2 overview of one spot of the cell field.

    (a) overview_map.png         many cells around the spot, the camera's field of view boxed
    (b) overview_structures.png  the structures in that box: cytoplasm height, nucleus, microtubules
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

# 16-stop viridis, for the cytoplasm height.
VIRIDIS = [(68, 1, 84), (72, 26, 108), (71, 47, 125), (65, 68, 135), (57, 86, 140), (49, 104, 142), (42, 120, 142),
           (35, 136, 142), (31, 152, 139), (34, 168, 132), (53, 183, 121), (84, 197, 104), (122, 209, 81),
           (165, 219, 54), (210, 226, 27), (253, 231, 37)]


def label_font(px):
    """A TrueType font with a micro sign (Pillow's built-in one has none); None if none is found."""
    for name in ("DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "arial.ttf",
                 "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            pass
    return None


def viridis(t):
    t = min(1.0, max(0.0, t)) * (len(VIRIDIS) - 1)
    i = min(int(t), len(VIRIDIS) - 2)
    f = t - i
    return tuple(int(round(a + (b - a) * f)) for a, b in zip(VIRIDIS[i], VIRIDIS[i + 1]))


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

    def __init__(self, geo):
        self.x0, self.y0 = geo["x0"], geo["y0"]
        self.s = PANEL * SS / (geo["x1"] - geo["x0"])
        self.img = Image.new("RGB", (PANEL * SS, PANEL * SS), BG)
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

def layer_cytoplasm(cv, cells):
    hmax = max((max(v[2] for v in c["mesh"]["v"]) for c in cells if c["mesh"]["v"]), default=1.0) or 1.0
    for c in cells:
        m = c["mesh"]
        R, n, v = m["rings"], m["n"], m["v"]
        for k in range(R):
            for i in range(n):
                j = (i + 1) % n
                q = [v[k * n + i], v[k * n + j], v[(k + 1) * n + j], v[(k + 1) * n + i]]
                h = sum(p[2] for p in q) / 4
                col = tuple(int(0.55 * ch) for ch in viridis(h / hmax))  # dimmed: the microtubules go on top
                cv.draw.polygon(cv.poly([(p[0], p[1]) for p in q]), fill=col)


def layer_outline(cv, cells):
    for c in cells:
        pts = cv.poly(c["outline"])
        cv.draw.line(pts + pts[:1], fill=(230, 230, 230, 200), width=2 * SS)


def layer_microtubules(cv, cells):
    zmax = max((max(p[2] for p in mt) for c in cells for mt in c["mts"] if mt), default=1.0) or 1.0
    for c in cells:
        for mt in c["mts"]:
            for a, b in zip(mt, mt[1:]):
                t = min(1.0, max(0.0, (a[2] + b[2]) / 2 / zmax))
                col = (int(60 + 195 * t), int(200 + 55 * t), 255, 150)  # cyan (low) -> white (high)
                cv.draw.line([cv.p(a[0], a[1]), cv.p(b[0], b[1])], fill=col, width=SS)


def layer_nucleus(cv, cells):
    for c in cells:
        pts = cv.poly(nucleus_poly(c["nucleus"]))
        cv.draw.polygon(pts, fill=(220, 60, 200, 90))
        cv.draw.line(pts + pts[:1], fill=(240, 120, 230, 230), width=2 * SS)


STRUCTURE_LAYERS = [layer_cytoplasm, layer_microtubules, layer_nucleus, layer_outline]


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
    cv = Canvas(geo)
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
            ("overview_structures.png", "The structures in the box: cytoplasm height, nucleus, microtubules"),
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
