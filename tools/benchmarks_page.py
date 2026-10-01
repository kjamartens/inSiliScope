#!/usr/bin/env python3
"""benchmarks.json (history written by bench.py) -> Markdown tables on stdout."""
import json, sys

hist = json.load(open(sys.argv[1], encoding="utf-8"))
print("# Benchmarks\n")
print("`insiliscope_cli` wall-clock (best of the repeats, includes process start and TIFF write), measured by the "
      "release workflow on a GitHub-hosted runner. Runner hardware varies between runs, so compare trends, not "
      "single numbers. Configurations: `tools/bench.py`.\n")
configs = [r["config"] for r in hist[-1]["results"]]
print("## Frames per second (higher is better)\n")
print("| version | date | CPUs | " + " | ".join(configs) + " |")
print("|---|---|---|" + "---|" * len(configs))
for h in reversed(hist):
    by = {r["config"]: r for r in h["results"]}
    cells = [("%.1f" % by[c]["frames_per_s"]) if c in by else "" for c in configs]
    print("| %s | %s | %s | %s |" % (h["version"], h["date"], h.get("cpus", ""), " | ".join(cells)))
print("\n## Machines\n")
for h in reversed(hist):
    print("- %s: %s" % (h["version"], h.get("machine", "")))
