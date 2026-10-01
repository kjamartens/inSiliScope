#!/usr/bin/env python3
"""Render gallery/manifest.json with insiliscope_cli into GIFs + a Markdown page.

usage: build_gallery.py --cli build/native/cli/insiliscope_cli --out site_gallery [--only id,id]
Needs: numpy, pillow, tifffile (pip install numpy pillow tifffile).
Outputs <out>/<id>.gif, <out>/<id>.tif (16-bit), <out>/gallery.json and <out>/gallery.md.
"""
import argparse, json, os, subprocess, sys, time
import numpy as np
import tifffile
from PIL import Image


def to_gif(stack, path, fps=10, scale=2):
    lo, hi = np.percentile(stack, 0.5), np.percentile(stack, 99.8)
    if hi <= lo:
        hi = lo + 1
    frames = []
    for f in stack:
        g = np.clip((f.astype(np.float32) - lo) / (hi - lo), 0, 1)
        g = (np.sqrt(g) * 255).astype(np.uint8)  # sqrt scale: shows the dim halo too
        im = Image.fromarray(g, "L").resize((g.shape[1] * scale, g.shape[0] * scale), Image.NEAREST)
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(__file__), "..", "gallery", "manifest.json"))
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    man = json.load(open(a.manifest, encoding="utf-8"))
    only = set(filter(None, a.only.split(",")))
    os.makedirs(a.out, exist_ok=True)
    meta = []
    for e in man["entries"]:
        if only and e["id"] not in only:
            continue
        opts = dict(man.get("defaults", {}))
        opts.update(e["options"])
        tif = os.path.join(a.out, e["id"] + ".tif")
        args = []
        for k, v in opts.items():
            args += ["--" + k, str(v)]
        t0 = time.time()
        r = subprocess.run([a.cli, "--out", tif] + args, capture_output=True, text=True)
        dt = time.time() - t0
        if r.returncode != 0:
            print("FAILED", e["id"], r.stderr[-400:], file=sys.stderr)
            sys.exit(1)
        stack = tifffile.imread(tif)
        if stack.ndim == 2:
            stack = stack[None]
        to_gif(stack, os.path.join(a.out, e["id"] + ".gif"))
        meta.append({**e, "resolved_options": opts, "frames_rendered": int(stack.shape[0]),
                     "seconds": round(dt, 2),
                     "cli": "insiliscope_cli --out %s.tif %s" % (e["id"], " ".join(args))})
        print("%-18s %3d frames  %.1fs" % (e["id"], stack.shape[0], dt))
    json.dump(meta, open(os.path.join(a.out, "gallery.json"), "w"), indent=1)
    lines = ["# Gallery", "",
             "Every movie on this page is produced by `insiliscope_cli` from `gallery/manifest.json` on each "
             "release. Frames use a square-root intensity scale. Seeds are fixed, so a re-run reproduces them.", ""]
    group = None
    for m in meta:
        if m["group"] != group:
            group = m["group"]
            lines += ["## " + group, ""]
        lines += ["### " + m["title"], "", m["text"], "",
                  "![%s](%s.gif)" % (m["title"], m["id"]), "", "`%s`" % m["cli"], ""]
    open(os.path.join(a.out, "gallery.md"), "w", encoding="utf-8").write("\n".join(lines))


if __name__ == "__main__":
    main()
