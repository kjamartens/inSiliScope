#!/usr/bin/env python3
"""The physics pages' figures, built from the code they document.

    python tools/build_physics_figures.py --cli <insiliscope_cli> [--out physics_figures] [--only id,id] [--jobs N]
    python tools/build_physics_figures.py --list
    python tools/build_physics_figures.py --inject <site docs dir> [--from physics_figures]

Every image and number comes from insiliscope_cli (the adapter's Simulation/ render code and the core; its read-only
diagnostic outputs --photons-out, --psf-out, --setup-json, ... in cli/scope_probes.h). The figures are rebuilt on every
Pages deploy (.github/workflows/pages.yml), so the docs show what the current code renders. Each figure is a function in
tools/physics_figures/<page>.py; it writes <out>/fig/*.png (and *.gif movies) and returns the Markdown that replaces
the marker `<!-- fig:<id> -->` (a line of its own) in docs/physics/*.md: <out>/snippets/<id>.md. <out>/figures.json lists the
figures, their cli commands and times.

--inject (tools/build_site.sh): copies <from>/fig to <site docs>/physics/fig and replaces the markers. Strict after a
full build: every marker needs a figure and every figure a marker. Without a build the markers stay HTML comments.

Figures run in --jobs processes (default: min(4, cores / 2)); the ones with timing rows (quality comparisons) run after
them, one at a time, so their times are not shared with another figure. Needs: numpy, pillow, tifffile, matplotlib.
"""
import argparse
import concurrent.futures as cf
import datetime
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from physics_figures import common  # noqa: E402
from physics_figures import (world, structures, light, photophysics, optics, quality, camera)  # noqa: E402,F401

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = re.compile(r"^<!-- fig:([a-z0-9-]+) -->[ \t]*$", re.M)


def git_rev():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True,
                              check=True).stdout.strip()
    except Exception:
        return os.environ.get("GITHUB_SHA", "")[:7]


def build_info():
    rev = git_rev()
    cores = os.cpu_count() or 1
    where = " at commit `%s`" % rev if rev else ""
    where += ", on %s with %d threads, %s" % (platform.system(), cores, datetime.date.today().isoformat())
    return {"rev": rev, "cores": cores, "system": platform.system(), "date": datetime.date.today().isoformat(),
            "where": where}


def run_one(fid, cli, out, cache_dir, info):
    """One figure, in this process: returns its record for figures.json (snippet written)."""
    common.BUILD_INFO.update(info)
    spec = next(s for s in common.REGISTRY if s.id == fid)
    ctx = common.Ctx(fid, cli, out, cache_dir, shared="pool-default")
    t0 = time.perf_counter()
    rec = {"id": fid, "page": spec.page, "timed": spec.timed}
    try:
        md = spec.fn(ctx).rstrip() + "\n\n" + common.provenance(ctx)
        with open(os.path.join(out, "snippets", fid + ".md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(md)
        rec["ok"] = True
    except Exception:
        rec["ok"] = False
        rec["error"] = traceback.format_exc()
    finally:
        ctx.close()
    rec["seconds"] = round(time.perf_counter() - t0, 2)
    rec["runs"] = [{"command": r.command, "wall": round(r.wall, 3), "times": r.times, "served": r.served}
                   for r in ctx.runs]
    rec["images"] = ctx.images
    return rec


def build(args):
    out = os.path.abspath(args.out)
    cli = os.path.abspath(args.cli)
    if not os.path.exists(cli) and os.path.exists(cli + ".exe"):
        cli += ".exe"
    if not os.path.exists(cli):
        sys.exit("no insiliscope_cli at " + cli)
    specs = common.REGISTRY
    if args.only:
        want = set(args.only.split(","))
        unknown = want - {s.id for s in specs}
        if unknown:
            sys.exit("unknown figure(s): " + ", ".join(sorted(unknown)))
        specs = [s for s in specs if s.id in want]
    if args.only:
        # A partial build replaces only its own figures.
        for d in ("fig", "snippets"):
            for name in os.listdir(os.path.join(out, d)) if os.path.isdir(os.path.join(out, d)) else []:
                if any(name.startswith(s.id + "-") or name.startswith(s.id + ".") for s in specs):
                    os.remove(os.path.join(out, d, name))
        for s in specs:
            shutil.rmtree(os.path.join(out, "tmp", s.id), ignore_errors=True)
    else:
        for d in ("fig", "snippets", "tmp"):
            shutil.rmtree(os.path.join(out, d), ignore_errors=True)
    os.makedirs(os.path.join(out, "snippets"), exist_ok=True)
    cache_dir = os.path.join(out, "cache")
    os.makedirs(cache_dir, exist_ok=True)
    info = build_info()
    jobs = args.jobs or max(1, min(4, (os.cpu_count() or 2) // 2))
    t0 = time.perf_counter()
    recs = []

    def report(rec):
        recs.append(rec)
        print("%-22s %6.1f s  %s" % (rec["id"], rec["seconds"], "ok" if rec["ok"] else "FAILED"), flush=True)
        if not rec["ok"]:
            print(rec["error"], file=sys.stderr, flush=True)

    # Before the figures: the packed cells of the spot every figure images, and the kernel pools (common.POOLS),
    # each computed alone (its compute time goes to pools.json for the oversampling table). A pool computed by the
    # same cli binary in an earlier build is kept, with the time measured then.
    tw = time.perf_counter()
    pools_path = os.path.join(cache_dir, "pools.json")
    old = common.load(pools_path) if os.path.exists(pools_path) else {}
    stamp = "%d-%d" % (os.path.getsize(cli), int(os.path.getmtime(cli)))
    pools, computed = {}, 0
    for name, opts in common.POOLS.items():
        kfile = os.path.join(cache_dir, "pool-" + name, "psf_kernel.bin")
        rec = old.get(name)
        if rec and rec.get("stamp") == stamp and rec.get("opts") == opts and os.path.exists(kfile):
            pools[name] = rec
            continue
        if os.path.exists(kfile):
            os.remove(kfile)
        r = common.Ctx("pool-" + name, cli, out, cache_dir).cli(
            dict(common.SPOT, size=256 if name == "default" else 64, prepare=1, **opts), fresh=True)
        m = re.search(r"PSF kernel ([\d.]+) s", r.stdout)
        pools[name] = {"stamp": stamp, "opts": opts, "kernel_s": float(m.group(1)) if m else None}
        computed += 1
    with open(pools_path, "w", encoding="utf-8") as f:
        json.dump(pools, f, indent=1)
    print("%-22s %6.1f s  (packed cells, %d PSF kernels, %d computed)" %
          ("prepare", time.perf_counter() - tw, len(common.POOLS), computed))
    untimed = [s for s in specs if not s.timed]
    timed = [s for s in specs if s.timed]
    if jobs > 1 and len(untimed) > 1:
        with cf.ProcessPoolExecutor(jobs) as pool:
            futs = [pool.submit(run_one, s.id, cli, out, cache_dir, info) for s in untimed]
            for f in cf.as_completed(futs):
                report(f.result())
    else:
        for s in untimed:
            report(run_one(s.id, cli, out, cache_dir, info))
    for s in timed:
        report(run_one(s.id, cli, out, cache_dir, info))
    total = time.perf_counter() - t0
    order = {s.id: i for i, s in enumerate(common.REGISTRY)}
    meta_path = os.path.join(out, "figures.json")
    if args.only and os.path.exists(meta_path):
        # Keep the other figures' records of an earlier build.
        mine = {r["id"] for r in recs}
        recs += [r for r in common.load(meta_path)["figures"] if r["id"] not in mine and r["id"] in order]
    recs.sort(key=lambda r: order[r["id"]])
    complete = {r["id"] for r in recs if r["ok"]} == set(order)
    doc = {"info": {k: v for k, v in info.items() if k != "where"}, "jobs": jobs, "seconds": round(total, 1),
           "complete": complete, "figures": recs}
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    if not args.keep_tmp:
        shutil.rmtree(os.path.join(out, "tmp"), ignore_errors=True)
        for name in os.listdir(cache_dir):   # the figures' kernel copies (the pools stay for the next build)
            if not name.startswith("pool-"):
                shutil.rmtree(os.path.join(cache_dir, name), ignore_errors=True)
    failed = [r["id"] for r in recs if not r["ok"] and r["id"] in {s.id for s in specs}]
    print("%d figures in %.1f s (%d jobs)%s" % (len(specs), total, jobs,
                                               ", FAILED: " + ", ".join(failed) if failed else ""))
    return 1 if failed else 0


def inject(site_docs, src):
    """Replace the markers in <site_docs>/physics/*.md with the snippets of <src>; copy the images."""
    meta_path = os.path.join(src, "figures.json")
    if not os.path.exists(meta_path):
        sys.exit("no figures in %s (run the build first)" % src)
    meta = common.load(meta_path)
    snippets = {}
    for name in os.listdir(os.path.join(src, "snippets")):
        if name.endswith(".md"):
            with open(os.path.join(src, "snippets", name), encoding="utf-8") as f:
                snippets[name[:-3]] = f.read()
    dst = os.path.join(site_docs, "physics", "fig")
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(os.path.join(src, "fig"), dst)
    used, missing = set(), []
    pages = sorted(n for n in os.listdir(os.path.join(site_docs, "physics")) if n.endswith(".md"))
    for name in pages:
        p = os.path.join(site_docs, "physics", name)
        with open(p, encoding="utf-8") as f:
            text = f.read()

        def sub(m):
            fid = m.group(1)
            if fid not in snippets:
                missing.append("%s (%s)" % (fid, name))
                return m.group(0)
            used.add(fid)
            return snippets[fid].rstrip("\n")

        new = MARKER.sub(sub, text)
        if new != text:
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
    unused = sorted(set(snippets) - used)
    if meta.get("complete"):
        problems = (["markers without a figure: " + ", ".join(missing)] if missing else []) + \
                   (["figures without a marker: " + ", ".join(unused)] if unused else [])
        if problems:
            sys.exit("inject: " + "; ".join(problems))
    elif missing or unused:
        print("inject (partial build): %d markers left as comments, %d figures unused" % (len(missing), len(unused)))
    print("injected %d figures into %s" % (len(used), os.path.join(site_docs, "physics")))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cli", help="insiliscope_cli executable")
    ap.add_argument("--out", default="physics_figures")
    ap.add_argument("--only", help="comma-separated figure ids")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--keep-tmp", action="store_true", help="keep the cli outputs (<out>/tmp)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--inject", metavar="SITE_DOCS")
    ap.add_argument("--from", dest="src", default="physics_figures")
    args = ap.parse_args()
    if args.list:
        for s in common.REGISTRY:
            print("%-22s %-22s%s" % (s.id, s.page, "  (timed)" if s.timed else ""))
        return 0
    if args.inject:
        return inject(args.inject, args.src)
    if not args.cli:
        ap.error("--cli is required to build")
    return build(args)


if __name__ == "__main__":
    sys.exit(main())
