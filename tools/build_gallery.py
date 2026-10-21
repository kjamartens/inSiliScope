#!/usr/bin/env python3
"""Render gallery/manifest.json with insiliscope_cli into GIFs + a Markdown page.

usage: build_gallery.py --cli build/native/cli/insiliscope_cli --out site_gallery [--only id,id]  ("overview" is an id too)
Needs: numpy, pillow, tifffile (pip install numpy pillow tifffile).
Outputs <out>/<id>.gif, <out>/<id>.tif (16-bit), <out>/gallery.json and <out>/gallery.md, and, when the manifest
has an "overview" block, the 2x2 overview panels (tools/build_overview.py) at the top of the page.
An entry with "z_sweep": [lo, hi, step] is a focus sweep: one frame per height (cli z) from lo to hi and back.
An entry with "gt": ["mt", "npc"] is a still image of the ground truth instead of a movie: the microtubules and/or
the nuclear pores of its field of view (insiliscope_cli --geometry-json, the same world the movies image).
A row with "show_time": true adds each movie's render time per frame to its label: the cli's own timing (ISC_TIMING=1,
Simulation/Timing.h), fl.total minus fl.setup (world, events, PSF kernel) over the frames, on the machine that builds
the gallery (frames render in parallel batches, so it is wall time per frame, not one core's).
The page follows the manifest's "sections": a story per section, then rows of side-by-side panels (an entry id or
[id, label]) with a caption; a row with "shared": true shows its panels on one display range (<row key>-<id>.gif).
Every entry must sit in some row (and every panel name an entry), so a new entry gets a place in the story.
"""
import argparse, json, os, subprocess, sys, time
import numpy as np
import tifffile
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_overview  # noqa: E402


def display_range(stacks):
    """The GIF display range: 0.5 and 99.8 percentiles over all the stacks (one row shared, or a single movie)."""
    v = np.concatenate([np.asarray(s, np.float32).ravel() for s in stacks])
    return np.percentile(v, 0.5), np.percentile(v, 99.8)


def to_gif(stack, path, fps=10, scale=None, rng=None):
    lo, hi = rng if rng else display_range([stack])
    if scale is None:
        scale = 2 if stack.shape[-1] < 512 else 1   # a 512 px movie (brightfield) is big enough as it is
    if hi <= lo:
        hi = lo + 1
    frames = []
    for f in stack:
        g = np.clip((f.astype(np.float32) - lo) / (hi - lo), 0, 1)
        g = (np.sqrt(g) * 255).astype(np.uint8)  # sqrt scale: shows the dim halo too
        im = Image.fromarray(g, "L").resize((g.shape[1] * scale, g.shape[0] * scale), Image.NEAREST)
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)


def draw_ground_truth(geo, layers, path):
    """The ground truth of a field of view in the viewer's colours (build_overview's layers): the microtubules ("mt")
    and the nuclear pores ("npc", a dot of about their size per pore) coloured by height, over the cell and nucleus."""
    cv = build_overview.Canvas(geo, build_overview.VIEWER_BG)
    cells = geo["cells"]
    build_overview.layer_cell(cv, cells)
    build_overview.layer_nucleus(cv, cells)
    build_overview.layer_outline(cv, cells)
    if "mt" in layers:
        build_overview.layer_microtubules(cv, cells)
    if "npc" in layers:
        build_overview.layer_pores(cv, cells, r=max(2.0, 0.06 * cv.s))   # ~120 nm across, at least a few pixels
    cv.scale_bar(2, "2 µm")
    cv.save(path)


def sweep_heights(lo, hi, step):
    """A focus sweep's heights: lo to hi and back (each end once)."""
    up = [round(lo + k * step, 4) for k in range(int(round((hi - lo) / step)) + 1)]
    return up, up + up[-2:0:-1]


def run_sweep(cli, out_dir, eid, args, sweep):
    """An entry with "z_sweep": [lo, hi, step]: one frame per focal height (cli z), rendered by one served cli
    (--serve keeps the world, PSF kernel and scenes between heights); the frames in sweep order, up and back."""
    heights, order = sweep_heights(*sweep)
    p = subprocess.Popen([cli, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True)
    frames = {}
    try:
        for z in heights:
            tif = os.path.join(out_dir, "%s.z%g.tif" % (eid, z))
            p.stdin.write("\t".join(["--out", tif] + args + ["--z", str(z), "--frames", "1"]) + "\n")
            p.stdin.flush()
            log = []
            while True:
                line = p.stdout.readline()
                if not line:
                    raise RuntimeError("insiliscope_cli --serve ended: " + "".join(log)[-400:])
                if line.startswith("@@isc-done "):
                    if line.split()[1] != "0":
                        raise RuntimeError("z %g failed: %s" % (z, "".join(log)[-400:]))
                    break
                log.append(line)
            frames[z] = tifffile.imread(tif)
            os.remove(tif)
    finally:
        p.stdin.close()
        p.wait()
    return np.stack([frames[z] for z in order]), heights


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(__file__), "..", "gallery", "manifest.json"))
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    a.cli = os.path.abspath(a.cli)   # Windows' CreateProcess does not resolve a relative path with forward slashes
    man = json.load(open(a.manifest, encoding="utf-8"))
    only = set(filter(None, a.only.split(",")))
    os.makedirs(a.out, exist_ok=True)
    overview = None
    if "overview" in man and (not only or "overview" in only):
        t0 = time.time()
        overview = build_overview.build(a.cli, a.out, man["overview"], man.get("defaults", {}).get("seed", 42))
        print("%-18s %.1fs" % ("overview", time.time() - t0))
    if overview is None and only and os.path.exists(os.path.join(a.out, "overview.json")):
        overview = json.load(open(os.path.join(a.out, "overview.json")))
    sections = man.get("sections", [])
    pid = lambda p: p if isinstance(p, str) else p[0]
    shown = {pid(p) for sec in sections for r in sec["rows"] for p in r["panels"]}
    ids = [e["id"] for e in man["entries"]]
    if shown - set(ids) or set(ids) - shown:
        print("manifest: panels without an entry: %s; entries in no row of 'sections': %s"
              % (sorted(shown - set(ids)) or "none", sorted(set(ids) - shown) or "none"), file=sys.stderr)
        sys.exit(1)
    timed = {pid(p) for sec in sections for r in sec["rows"] if r.get("show_time") for p in r["panels"]}
    meta = {}
    if only and os.path.exists(os.path.join(a.out, "gallery.json")):   # re-render a few: keep the others' metadata
        prev = {m["id"]: m for m in json.load(open(os.path.join(a.out, "gallery.json")))}
        meta = {i: prev[i] for i in ids if i in prev and i not in only}
    for e in man["entries"]:
        if only and e["id"] not in only:
            continue
        opts = dict(man.get("defaults", {}))
        opts.update(e["options"])
        if e.get("gt"):
            fov_um = float(opts.get("size", 128)) * float(opts.get("pixel-nm", 100)) / 1000
            gopts = {k: v for k, v in opts.items() if k in ("seed", "world-seed", "x", "y") or k.startswith("p.")}
            t0 = time.time()
            draw_ground_truth(build_overview.cli_geometry(a.cli, gopts, fov_um, True), e["gt"],
                              os.path.join(a.out, e["id"] + ".png"))
            cli = "insiliscope_cli --geometry-json %s.json --geometry-um %g --geometry-detail 1 %s" % (
                e["id"], fov_um, " ".join("--%s %s" % kv for kv in gopts.items()))
            meta[e["id"]] = {**e, "resolved_options": gopts, "frames_rendered": 0, "seconds": round(time.time() - t0, 2),
                             "cli": cli, "file": e["id"] + ".png"}
            print("%-22s ground truth  %.1fs" % (e["id"], time.time() - t0))
            continue
        tif = os.path.join(a.out, e["id"] + ".tif")
        sweep = e.get("z_sweep")
        if sweep:
            opts = {k: v for k, v in opts.items() if k not in ("z", "frames")}
        args = []
        for k, v in opts.items():
            args += ["--" + k, str(v)]
        t0 = time.time()
        env = None
        if sweep:
            try:
                stack, heights = run_sweep(a.cli, a.out, e["id"], args, sweep)
            except RuntimeError as err:
                print("FAILED", e["id"], err, file=sys.stderr)
                sys.exit(1)
            tifffile.imwrite(tif, stack)
            cli = "for z in %s: insiliscope_cli --out %s.z<z>.tif %s --z <z> --frames 1" % (
                " ".join("%g" % z for z in heights), e["id"], " ".join(args))
        else:
            env = dict(os.environ, ISC_TIMING="1") if e["id"] in timed else None
            r = subprocess.run([a.cli, "--out", tif] + args, capture_output=True, text=True, env=env)
            if r.returncode != 0:
                print("FAILED", e["id"], r.stderr[-400:], file=sys.stderr)
                sys.exit(1)
            stack = tifffile.imread(tif)
            cli = "insiliscope_cli --out %s.tif %s" % (e["id"], " ".join(args))
        dt = time.time() - t0
        ms_frame = None
        if env:
            t = {}
            for line in (r.stdout + r.stderr).splitlines():
                w = line.split()
                if len(w) >= 3 and w[0] == "ISC_TIMING" and w[1] in ("fl.total", "fl.setup"):
                    t[w[1]] = float(w[2])
            if len(t) == 2:
                ms_frame = (t["fl.total"] - t["fl.setup"]) / int(opts.get("frames", 1000)) * 1000
        if stack.ndim == 2:
            stack = stack[None]
        to_gif(stack, os.path.join(a.out, e["id"] + ".gif"), fps=6 if sweep else 10)
        meta[e["id"]] = {**e, "resolved_options": opts, "frames_rendered": int(stack.shape[0]),
                         "seconds": round(dt, 2), "cli": cli, "fps": 6 if sweep else 10,
                         "file": e["id"] + ".gif", "ms_per_frame": ms_frame}
        print("%-22s %3d frames  %.1fs" % (e["id"], stack.shape[0], dt))
    meta = {i: meta[i] for i in ids if i in meta}   # manifest order
    json.dump(list(meta.values()), open(os.path.join(a.out, "gallery.json"), "w"), indent=1)

    lines = ["# Gallery", "",
             "What the simulation can show, one topic at a time, mostly as side-by-side comparisons. Every movie is "
             "made by `insiliscope_cli` from `gallery/manifest.json` on each release, with fixed seeds, so a re-run "
             "reproduces it; frames use a square-root intensity scale. Open *How these movies were made* under a "
             "section for each movie's settings and command line, or try any of it live in the "
             "[viewer](../../){ target=\"_blank\" }.", ""]
    if overview:
        lines += ["## What inSiliScope is", "",
                  "One spot of the endless cell field, four ways: where it is, what is there, and how a widefield "
                  "and an SMLM camera see it.", "", build_overview.grid_html(""), "",
                  "`%s`  " % overview["wf_cli"], "`%s`" % overview["sr_cli"], ""]
    for si, sec in enumerate(sections):
        lines += ["## " + sec["title"], "", sec["story"], ""]
        used = []
        for ri, row in enumerate(sec["rows"]):
            panels = [(pid(p), p[1] if not isinstance(p, str) else None) for p in row["panels"]]
            panels = [(i, lab or meta[i]["title"]) for i, lab in panels if i in meta]
            if not panels:
                continue
            if row.get("show_time"):
                panels = [(i, lab + (" · %.1f ms/frame" % meta[i]["ms_per_frame"] if meta[i].get("ms_per_frame") is not None else ""))
                          for i, lab in panels]
            gifs = {i: meta[i]["file"] for i, _ in panels}
            if row.get("shared") and len(panels) > 1:
                rng = display_range([tifffile.imread(os.path.join(a.out, i + ".tif")) for i, _ in panels])
                for i, _ in panels:
                    gifs[i] = "s%dr%d-%s.gif" % (si + 1, ri + 1, i)
                    to_gif(tifffile.imread(os.path.join(a.out, i + ".tif")), os.path.join(a.out, gifs[i]),
                           fps=meta[i]["fps"], rng=rng)
            figs = ['  <figure><img src="%s" alt="%s" loading="lazy"><figcaption>%s</figcaption></figure>'
                    % (gifs[i], lab, lab) for i, lab in panels]
            # at least three columns: a pair or a single movie stays at the size of a three-panel row
            lines += ['<div class="isc-row" style="--isc-n: %d">' % max(3, len(panels))] + figs + ["</div>", ""]
            if row.get("caption"):
                cap = row["caption"] + (" *(One display range for the row.)*" if row.get("shared") and len(panels) > 1 else "")
                lines += ['<p class="isc-row-cap" markdown>%s</p>' % cap, ""]
            used += [i for i, _ in panels if i not in used]
        if used:
            lines += ['??? info "How these were made"', ""]
            for i in used:
                m = meta[i]
                lines += ["    **%s.** %s  " % (m["title"], m["text"]), "    `%s`" % m["cli"], ""]
    open(os.path.join(a.out, "gallery.md"), "w", encoding="utf-8").write("\n".join(lines))


if __name__ == "__main__":
    main()
