#!/usr/bin/env python3
"""benchmarks.json (history written by bench.py, plus the Micro-Manager part of bench_live.py --history) -> the
project site's Benchmarks page (Markdown with inline HTML bars) on stdout."""
import html
import json
import sys

# What each phase of a live Micro-Manager frame is (Simulation/Timing.h names). The stacked bar of a frame is made
# of the FRAME phases (they follow each other on the producer thread); the others are inside one of them.
FRAME = [
    ("live.state", "read settings, pose, drift"),
    ("live.ahead-wait", "wait for a render-ahead batch"),
    ("live.spec+clock", "build the scene spec, illumination clocks"),
    ("live.begin", "movie setup (light path, kernels, dye queries)"),
    ("live.render", "render the photons (splat / GPU)"),
    ("live.lamp", "BrightField image"),
    ("live.noise", "camera noise"),
    ("live.publish", "hand the frame to the camera"),
]
COLORS = ["#8d99ae", "#bab0ac", "#4c78a8", "#f58518", "#e45756", "#72b7b2", "#54a24b", "#b279a2"]
DESCRIBE = {
    "live.frame": "producer: one frame from reading the state to publishing it (the render time)",
    "live.idle": "producer: waiting for the next exposure slot",
    "live.prefetch": "producer: pre-loading dyes around the FOV in idle time",
    "live.zseq-wait": "producer: waiting for the consumer in a hardware z stack",
    "live.ahead-frame": "producer: a frame taken from the render-ahead queue (count)",
    "live.ahead-submit": "producer: hand the next batch to the render-ahead helper",
    "live.ahead-flush": "producer: queued frames dropped on a change of state (count)",
    "live.ahead-drop": "producer: a queued frame for a slot already shown (count)",
    "bf.scene": "BrightField: scene built (a new pose or setting)",
    "bf.image": "BrightField: an image at a new focus, on the producer",
    "bf.prefetch-image": "BrightField: an image of a focus ahead, on the prefetch thread",
    "bf.prefetch-wait": "BrightField: the producer waits for the focus being prefetched",
    "bf.focus-cached": "BrightField: a frame whose focus image was ready (count)",
    "ahead.batch": "render-ahead helper: one batch of frames (setup, render, noise)",
    "ahead.cpu.render+noise": "render-ahead helper: the batch's frames on the CPU",
    "ahead.gpu.populations": "render-ahead helper: the batch's continuous populations (CPU) for the GPU",
    "ahead.gpu.collect": "render-ahead helper: emitters of the batch's frames",
    "ahead.gpu.splat+noise": "render-ahead helper: the batch's frames in one GPU dispatch",
    "mm.wait-frame (sequence)": "sequence thread: waiting for a fresh frame",
    "mm.wait-frame (snap)": "snap: waiting for a frame started after the call",
    "mm.frame-age": "age of a frame when taken (publish -> take)",
    "mm.copy+history": "copy into the camera buffer, add its light to the illumination history",
    "mm.insert-image": "MMCore InsertImage (circular buffer)",
    "mm.snap": "SnapImage, call to return",
    "fl.setup": "movie setup total",
    "fl.make-setup": "camera, light path, labels",
    "fl.configure": "configure the shared cell field",
    "fl.clock-regions": "distinct illumination clocks in the FOV",
    "fl.history": "the clock regions' rate histories (past light paths and labels)",
    "fl.psf-kernels": "PSF kernels (memo hit, or computed)",
    "fl.events-query": "blink events of the frame from the core",
    "fl.events-sort": "group and bucket the events",
    "fl.dye-counts": "count the dyes of continuous populations",
    "fl.windows": "per-dye emission windows",
    "fl.mean-field-scene": "mean-field scene update",
    "fl.mean-field-weights": "per-column dye clocks -> photons",
    "fl.mean-field-render": "weighted mean-field image",
    "fl.frame.blinks (batches)": "splat the blinks, or bin them (binned FFT) in dense frames (CPU)",
    "fl.frame.continuous": "add the continuous populations",
    "gpu.populations": "GPU path: the frame's continuous populations (CPU)",
    "gpu.collect": "GPU: emitters of the frame",
    "gpu.splat+noise": "GPU: splat, noise and readback",
    "psf.compute": "PSF kernel computed",
    "psf.zernike-planes": "PSF: pupil -> planes (chirp-Z)",
    "psf.normalize+block-sums": "PSF: normalise, block sums for the splat",
    "init.camera": "Camera Initialize",
    "init.preload (background)": "PSF preload thread at init",
}


def bar(parts, total_ms, scale_ms):
    """A stacked horizontal bar: parts = [(name, ms)], width relative to scale_ms."""
    segs = []
    for (name, ms), color in zip(parts, COLORS):
        if ms <= 0:
            continue
        w = 100.0 * ms / scale_ms
        segs.append(f'<span title="{html.escape(name)}: {ms:.2f} ms" style="display:inline-block;height:14px;'
                    f'width:{w:.2f}%;background:{color}"></span>')
    return (f'<div style="width:100%;background:var(--md-default-fg-color--lightest);white-space:nowrap">'
            f'{"".join(segs)}</div>')


def cli_section(hist):
    rows = [h for h in hist if h.get("results")]
    if not rows:
        return
    print("## insiliscope_cli\n")
    print("`insiliscope_cli` wall-clock (best of the repeats, includes process start and TIFF write), measured by the "
          "release workflow on a GitHub-hosted Linux runner. Runner hardware varies between runs, so compare trends, "
          "not single numbers. Configurations: `tools/bench.py`.\n")
    configs = [r["config"] for r in rows[-1]["results"]]
    print("### Frames per second (higher is better)\n")
    print("| version | date | CPUs | " + " | ".join(configs) + " |")
    print("|---|---|---|" + "---|" * len(configs))
    for h in reversed(rows):
        by = {r["config"]: r for r in h["results"]}
        cells = [("%.1f" % by[c]["frames_per_s"]) if c in by else "" for c in configs]
        print("| %s | %s | %s | %s |" % (h["version"], h["date"], h.get("cpus", ""), " | ".join(cells)))
    print()


def mm_section(hist):
    rows = [h for h in hist if isinstance(h.get("mm"), dict)]
    if not rows:
        return
    latest = rows[-1]["mm"]
    m = latest.get("machine", {})
    print("## Micro-Manager live (MMCore)\n")
    print(f"The adapter as Micro-Manager drives it: the shipped `inSiliScope_{latest['config']}.cfg` at "
          f"{latest['fov']} x {latest['fov']} px, every Channel preset, a continuous sequence acquisition (Micro-"
          "Manager's live view) per exposure, measured through pymmcore-plus by `tools/bench_live.py --profile` on "
          f"the release workflow's Windows runner ({m.get('cpus', '?')} CPUs; renderer: "
          f"{html.escape(latest.get('gpu') or 'unknown')}). "
          + ("" if "GPU:" in (latest.get("gpu") or "") else
             "Without a usable GPU every frame renders on the CPU; a desktop GPU makes DNA-PAINT faster. ")
          + "The phase times come from the adapter itself (`Simulation/Timing.h`, Camera `Test_ProfileWriteTo`).\n")
    exps = sorted({r["exposureMs"] for r in latest["live"]})
    channels = list(dict.fromkeys(r["channel"] for r in latest["live"]))
    print("### Frames per second, by version\n")
    ref = 10.0 if 10.0 in exps else exps[0]
    print(f"At {ref:g} ms exposure (at most {1000 / ref:g} fps).\n")
    print("| version | date | " + " | ".join(channels) + " | first image (ms) |")
    print("|---|---|" + "---|" * len(channels) + "---|")
    for h in reversed(rows):
        by = {(r["channel"], r["exposureMs"]): r for r in h["mm"]["live"]}
        cells = [("%.1f" % by[(c, ref)]["fps"]) if (c, ref) in by else "" for c in channels]
        print("| %s | %s | %s | %.0f |" % (h["version"], h["mm"].get("date", h.get("date", "")), " | ".join(cells),
                                            h["mm"].get("firstSnapMs", 0)))
    print()
    print(f"### Latest release ({rows[-1]['version']}): frame rate and render time\n")
    print("fps achieved / the exposure's maximum; render = the producer's time per frame (ms); gap = the longest "
          "interval between two frames (ms).\n")
    print("| Channel | " + " | ".join(f"{e:g} ms" for e in exps) + " |")
    print("|---|" + "---|" * len(exps))
    by = {(r["channel"], r["exposureMs"]): r for r in latest["live"]}
    for c in channels:
        cells = []
        for e in exps:
            r = by.get((c, e))
            cells.append("" if not r else f"{r['fps']:.1f} / {r['maxFps']:.0f}, render {r['renderMs']:.1f}, "
                                          f"gap {r['worstGapMs']:.0f}")
        print(f"| {c} | " + " | ".join(cells) + " |")
    print()

    # Where the time of a frame goes.
    profiled = [r for r in latest["live"] if r.get("profile")]
    if profiled:
        print("### Where a live frame's time goes\n")
        print("Time per published frame on the producer thread, at the exposure above, by phase (frames from the "
              "render-ahead queue were rendered by its helper thread meanwhile, `ahead.batch`):\n")
        legend = " ".join(f'<span style="display:inline-block;width:10px;height:10px;background:{col}"></span> '
                          f"`{n}` {html.escape(d)}" for (n, d), col in zip(FRAME, COLORS))
        print(legend + "\n")
        sel = [r for r in profiled if r["exposureMs"] == ref] or profiled
        totals = {}
        for r in sel:
            ph = r["profile"]["phases"]
            frames = max(1, ph.get("live.frame", {}).get("count", 0))
            totals[r["channel"]] = [(n, ph.get(n, {}).get("totalMs", 0.0) / frames) for n, _ in FRAME]
        scale = max([sum(ms for _, ms in t) for t in totals.values()] + [1e-9])
        print('<div>')
        for r in sel:
            parts = totals[r["channel"]]
            tot = sum(ms for _, ms in parts)
            print(f'<div style="display:flex;align-items:center;gap:0.8em;margin:0.3em 0">'
                  f'<span style="flex:0 0 11em">{html.escape(r["channel"])}</span>'
                  f'<span style="flex:1 1 auto">{bar(parts, tot, scale)}</span>'
                  f'<span style="flex:0 0 9em;text-align:right">{tot:.1f} ms, {r["fps"]:.1f} fps</span></div>')
        print("</div>\n")
        for r in sel:
            ph = r["profile"]["phases"]
            print(f'??? note "{r["channel"]}, {r["exposureMs"]:g} ms: every phase"\n')
            print("    | phase | what | per frame (ms) | longest (ms) | count |")
            print("    |---|---|---|---|---|")
            for k, v in sorted(ph.items(), key=lambda kv: -kv[1]["meanMs"]):
                if v["meanMs"] < 0.01 and v["maxMs"] < 0.05:
                    continue
                print(f"    | `{k}` | {DESCRIBE.get(k, dict(FRAME).get(k, ''))} | {v['meanMs']:.2f} | "
                      f"{v['maxMs']:.2f} | {v['count']} |")
            print()

    lat = latest.get("latencyMs", {})
    if lat:
        print("### Latencies (call -> image, 20 ms exposure)\n")
        prof = (latest.get("profiles") or {})
        acts = prof.get("actions", {})
        print("| action | ms | largest phases |")
        print("|---|---|---|")
        names = {"snap20ms": "snap", "stageMove30um": "30 um stage move + snap", "objectiveSwitch":
                 "objective switch + snap", "objectiveSwitchBack": "objective switch back + snap",
                 "zMove1um": "1 um focus move + snap"}
        rows_lat = [("first image after loading the configuration", latest.get("firstSnapMs", 0),
                     (prof.get("startup") or {}).get("phases", {}))]
        rows_lat += [(names.get(k, k), v, (acts.get(k) or {}).get("phases", {})) for k, v in lat.items()]
        for ch, p in (prof.get("channelSwitch") or {}).items():
            r = next((x for x in latest["live"] if x["channel"] == ch), None)
            if r:
                rows_lat.append((f"Channel switch to {ch} + snap", r["switchMs"], p.get("phases", {})))
        for name, ms, phases in rows_lat:
            top = sorted(((k, v["totalMs"]) for k, v in phases.items()
                          if not k.startswith("mm.") and k not in ("live.frame", "fl.total", "fl.setup")),
                         key=lambda kv: -kv[1])[:4]
            print(f"| {name} | {ms:.0f} | " + ", ".join(f"`{k}` {t:.0f}" for k, t in top) + " |")
        print()


def main():
    hist = json.load(open(sys.argv[1], encoding="utf-8"))
    print("# Benchmarks\n")
    print("Measured on each release. Micro-Manager first (what a user of the adapter sees), then the command-line "
          "renderer.\n")
    mm_section(hist)
    cli_section(hist)
    print("## Machines\n")
    for h in reversed(hist):
        mm = h.get("mm", {}).get("machine", {})
        print("- %s: cli %s%s" % (h["version"], h.get("machine", "") or "-",
                                  f"; Micro-Manager {mm.get('platform', '')}, {mm.get('cpus', '?')} CPUs, "
                                  f"pymmcore {mm.get('pymmcore', '?')}" if mm else ""))


if __name__ == "__main__":
    main()
