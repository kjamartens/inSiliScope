"""Live speed of the inSiliScope adapter as a user meets it: a shipped configuration, every Channel preset, a few
exposures. Per case: the achieved frame rate of a continuous sequence acquisition (what Micro-Manager's live view
shows) against the exposure's 1000/exposure, and the producer's render time per frame (Test_LiveRenderMs). Then the
latency of everyday actions (snap, Channel switch, Objective switch, stage move: call -> image).

  ADAPTER_DIR=<dll dir> python tools/bench_live.py [--config Basic] [--fov 256] [--exposures 5,10,20,50]
                                                   [--seconds 1.5] [--json out.json]
(about 2 min with the defaults)
"""

import argparse
import json
import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_mm_configs import OUT  # noqa: E402
from isc_mm import search_paths  # noqa: E402

os.environ["ISC_TEST"] = "1"   # the Test_LiveRenderMs readout (after gen_mm_configs, which sets 0)


def fps_run(core, seconds):
    """Frames per second of a continuous acquisition, after a 0.5 s warm-up; the render time at its end."""
    core.startContinuousSequenceAcquisition(0)
    t_end_warm = time.perf_counter() + 0.5
    while time.perf_counter() < t_end_warm:
        while core.getRemainingImageCount():
            core.popNextImage()
        time.sleep(0.0005)
    stamps = []
    t_end = time.perf_counter() + seconds
    while time.perf_counter() < t_end:
        while core.getRemainingImageCount():
            core.popNextImage()
            stamps.append(time.perf_counter())
        time.sleep(0.0005)
    core.stopSequenceAcquisition()
    render = float(core.getProperty("Camera", "Test_LiveRenderMs"))
    prefetch = core.getProperty("Camera", "Test_LivePrefetchMs")
    gaps = sorted(b - a for a, b in zip(stamps, stamps[1:]))
    worst = gaps[-1] * 1000 if gaps else 0.0
    if len(stamps) < 2:
        return 0.0, render, len(stamps), prefetch, worst
    return (len(stamps) - 1) / (stamps[-1] - stamps[0]), render, len(stamps), prefetch, worst


def timed(fn):
    t = time.perf_counter()
    fn()
    return (time.perf_counter() - t) * 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="Basic")
    ap.add_argument("--fov", default="256")
    ap.add_argument("--exposures", default="5,10,20,50")
    ap.add_argument("--seconds", type=float, default=1.5)
    ap.add_argument("--channels", default="", help="comma-separated Channel presets (default: all)")
    ap.add_argument("--json", default="")
    ap.add_argument("--verbose", action="store_true", help="also the worst frame gap and the longest prefetch/budget")
    a = ap.parse_args()
    from pymmcore_plus import CMMCorePlus

    cfg = (OUT / f"inSiliScope_{a.config}.cfg").read_text(encoding="utf-8")
    cfg = cfg.replace("Property,Camera,FovSize,256x256", f"Property,Camera,FovSize,{a.fov}x{a.fov}")
    tmp = Path(tempfile.mkdtemp()) / "bench.cfg"
    tmp.write_text(cfg, encoding="utf-8")
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths())
    t0 = time.perf_counter()
    core.loadSystemConfiguration(str(tmp))
    load_ms = (time.perf_counter() - t0) * 1000
    first_snap = timed(core.snapImage)
    print(f"{a.config}, {a.fov} px: load {load_ms:.0f} ms, first snap {first_snap:.0f} ms")

    rows = []
    channels = [c for c in core.getAvailableConfigs("Channel") if not a.channels or c in a.channels.split(",")]
    exposures = [float(e) for e in a.exposures.split(",")]
    print(f"{'channel':34s} " + " ".join(f"{e:>5g} ms: fps/max  render" for e in exposures))
    for ch in channels:
        switch = timed(lambda: (core.setConfig("Channel", ch), core.waitForSystem(), core.snapImage()))
        cells = []
        for e in exposures:
            core.setExposure(e)
            fps, render, n, prefetch, worst = fps_run(core, a.seconds)
            rows.append({"channel": ch, "exposureMs": e, "fps": fps, "maxFps": 1000.0 / e, "renderMs": render,
                         "frames": n, "switchMs": switch, "prefetchMs": prefetch, "worstGapMs": worst})
            cells.append(f"{fps:6.1f}/{1000 / e:<5.0f} {render:6.1f}" + (f" [gap {worst:.0f}, pf {prefetch}]"
                                                                         if a.verbose else ""))
        print(f"{ch:34s} " + "  ".join(cells) + f"   (switch + snap {switch:.0f} ms)")

    core.setExposure(20.0)
    core.setConfig("Channel", channels[0])
    snaps = sorted(timed(core.snapImage) for _ in range(5))
    xy = core.getXYStageDevice()
    x0, y0 = core.getXYPosition(xy)
    moves = []
    for k in range(3):
        moves.append(timed(lambda: (core.setXYPosition(xy, x0 + 30.0 * (k + 1), y0), core.waitForDevice(xy),
                                    core.snapImage())))
    objs = core.getAvailableConfigs("Objective")
    obj = timed(lambda: (core.setConfig("Objective", objs[-1]), core.waitForSystem(), core.snapImage()))
    obj_back = timed(lambda: (core.setConfig("Objective", objs[0]), core.waitForSystem(), core.snapImage()))
    z = core.getFocusDevice()
    zmove = timed(lambda: (core.setPosition(z, core.getPosition(z) + 1.0), core.waitForDevice(z), core.snapImage()))
    lat = {"snap20ms": snaps[2], "stageMove30um": sorted(moves)[1], "objectiveSwitch": obj,
           "objectiveSwitchBack": obj_back, "zMove1um": zmove}
    print("latency (ms, call -> image, 20 ms exposure): " + ", ".join(f"{k} {v:.0f}" for k, v in lat.items()))
    core.unloadAllDevices()
    if a.json:
        Path(a.json).write_text(json.dumps({"config": a.config, "fov": a.fov, "loadMs": load_ms,
                                            "firstSnapMs": first_snap, "live": rows, "latencyMs": lat}, indent=1))


if __name__ == "__main__":
    main()
