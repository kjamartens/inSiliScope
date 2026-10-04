#!/usr/bin/env python3
"""Time insiliscope_cli at fixed configurations; appends one entry per version to a history file.

usage: bench.py --cli build/native/cli/insiliscope_cli --version v0.1.0 --out benchmarks.json
"""
import argparse, json, os, platform, subprocess, tempfile, time

CONFIGS = [
    ("sr-128px-200f", {"size": 128, "frames": 200}),
    ("sr-256px-200f", {"size": 256, "frames": 200}),
    ("sr-dense-128px", {"size": 128, "frames": 200, "milli-activation-rate": 14.3}),
    ("wf-128px-20f", {"size": 128, "frames": 20, "modality": "WideField"}),
    ("wf-256px-20f", {"size": 256, "frames": 20, "modality": "WideField"}),
    ("wf-128px-x2", {"size": 128, "frames": 20, "modality": "WideField", "wf-upscale": 2}),
    ("sr-128px-1000f", {"size": 128, "frames": 1000}),
    ("wf-256px-200f", {"size": 256, "frames": 200, "modality": "WideField"}),
    ("bf-256px-q3", {"size": 256, "frames": 3, "modality": "BrightField", "bf-quality": 3}),
    ("bf-256px-q4", {"size": 256, "frames": 3, "modality": "BrightField", "bf-quality": 4}),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--version", default="dev")
    ap.add_argument("--out", required=True)
    ap.add_argument("--repeats", type=int, default=3)
    a = ap.parse_args()
    results = []
    with tempfile.TemporaryDirectory() as d:
        for name, opts in CONFIGS:
            ts = []
            for _ in range(a.repeats):
                cmd = [a.cli, "--out", os.path.join(d, "b.tif"), "--seed", "42"]
                for k, v in opts.items():
                    cmd += ["--" + k, str(v)]
                t0 = time.perf_counter()
                subprocess.run(cmd, check=True, capture_output=True)
                ts.append(time.perf_counter() - t0)
            results.append({"config": name, "options": opts, "best_s": round(min(ts), 3),
                            "median_s": round(sorted(ts)[len(ts) // 2], 3),
                            "frames_per_s": round(opts["frames"] / min(ts), 2)})
            print("%-16s best %.2fs  %.1f frames/s" % (name, min(ts), opts["frames"] / min(ts)))
    entry = {"version": a.version, "date": time.strftime("%Y-%m-%d"), "machine": platform.platform(),
             "cpus": os.cpu_count(), "results": results}
    hist = json.load(open(a.out)) if os.path.exists(a.out) else []
    hist = [h for h in hist if h["version"] != a.version] + [entry]
    json.dump(hist, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
