"""Live speed of the inSiliScope adapter as a user meets it: a shipped configuration, every Channel preset, a few
exposures. Per case: the achieved frame rate of a continuous sequence acquisition (what Micro-Manager's live view
shows) against the exposure's 1000/exposure, and the producer's render time per frame (Test_LiveRenderMs). Then the
latency of everyday actions (snap, Channel switch, Objective switch, stage move: call -> image).

--profile also records where the time goes: the adapter's phase profile (Simulation/Timing.h, collected in memory with
ISC_PROFILE=1 and written by the Camera's Test_ProfileWriteTo) for the configuration's load and first image, every
live case and every action. --history adds the result to a benchmarks.json entry (the release workflow's; rendered on
the project site's Benchmarks page by tools/benchmarks_page.py).

  ADAPTER_DIR=<dll dir> python tools/bench_live.py [--config Basic] [--fov 256] [--exposures 5,10,20,50]
                                                   [--seconds 1.5] [--profile] [--json out.json]
                                                   [--history benchmarks.json --version v0.2.0]
(about 2-3 min with the defaults; only the inSiliScope DLL is needed for the Basic configuration)
"""

import argparse
import json
import os
import platform
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_mm_configs import OUT  # noqa: E402
from isc_mm import search_paths  # noqa: E402

os.environ["ISC_TEST"] = "1"   # the Test_ rows (after gen_mm_configs, which sets 0)


class Profiler:
    """The adapter's phase profile: take() returns the phases since the last take (and starts afresh)."""

    def __init__(self, core, on):
        self.core, self.on = core, on
        self.path = Path(tempfile.mkdtemp()) / "profile.json"

    def take(self):
        if not self.on:
            return None
        self.core.setProperty("Camera", "Test_ProfileWriteTo", str(self.path))
        self.core.setProperty("Camera", "Test_ProfileWriteTo", "")
        return json.loads(self.path.read_text(encoding="utf-8"))


def fps_run(core, seconds, prof):
    """Frames per second of a continuous acquisition, after a 0.5 s warm-up; the render time at its end and the phase
    profile of the measured part."""
    core.startContinuousSequenceAcquisition(0)
    t_end_warm = time.perf_counter() + 0.5
    while time.perf_counter() < t_end_warm:
        while core.getRemainingImageCount():
            core.popNextImage()
        time.sleep(0.0005)
    prof.take()
    stamps = []
    t_end = time.perf_counter() + seconds
    while time.perf_counter() < t_end:
        while core.getRemainingImageCount():
            core.popNextImage()
            stamps.append(time.perf_counter())
        time.sleep(0.0005)
    profile = prof.take()
    core.stopSequenceAcquisition()
    render = float(core.getProperty("Camera", "Test_LiveRenderMs"))
    prefetch = core.getProperty("Camera", "Test_LivePrefetchMs")
    gaps = sorted(b - a for a, b in zip(stamps, stamps[1:]))
    worst = gaps[-1] * 1000 if gaps else 0.0
    fps = (len(stamps) - 1) / (stamps[-1] - stamps[0]) if len(stamps) >= 2 else 0.0
    return {"fps": fps, "renderMs": render, "frames": len(stamps), "prefetchMs": prefetch, "worstGapMs": worst,
            "profile": profile}


def timed(fn):
    t = time.perf_counter()
    fn()
    return (time.perf_counter() - t) * 1000.0


def machine():
    m = {"platform": platform.platform(), "cpus": os.cpu_count(), "processor": platform.processor()}
    try:
        import pymmcore
        m["pymmcore"] = pymmcore.__version__
    except Exception:
        pass
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="Basic")
    ap.add_argument("--fov", default="256")
    ap.add_argument("--exposures", default="5,10,20,50")
    ap.add_argument("--seconds", type=float, default=1.5)
    ap.add_argument("--channels", default="", help="comma-separated Channel presets (default: all)")
    ap.add_argument("--profile", action="store_true", help="record the adapter's phase profile per case")
    ap.add_argument("--json", default="")
    ap.add_argument("--history", default="", help="benchmarks.json to add this run to (entry of --version)")
    ap.add_argument("--version", default="dev")
    ap.add_argument("--verbose", action="store_true", help="also the worst frame gap and the longest prefetch/budget")
    a = ap.parse_args()
    if a.profile:
        os.environ["ISC_PROFILE"] = "1"   # before the adapter loads: the load and first image are profiled too
    from pymmcore_plus import CMMCorePlus

    cfg = (OUT / f"inSiliScope_{a.config}.cfg").read_text(encoding="utf-8")
    cfg = cfg.replace("Property,Camera,FovSize,256x256", f"Property,Camera,FovSize,{a.fov}x{a.fov}")
    tmp = Path(tempfile.mkdtemp()) / "bench.cfg"
    tmp.write_text(cfg, encoding="utf-8")
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths(require_mm=False))
    t0 = time.perf_counter()
    core.loadSystemConfiguration(str(tmp))
    load_ms = (time.perf_counter() - t0) * 1000
    prof = Profiler(core, a.profile)
    first_snap = timed(core.snapImage)
    startup_profile = prof.take()
    gpu = core.getProperty("Renderer", "Test_GpuStatus") if core.hasProperty("Renderer", "Test_GpuStatus") else ""
    print(f"{a.config}, {a.fov} px: load {load_ms:.0f} ms, first snap {first_snap:.0f} ms" +
          (f", renderer: {gpu}" if gpu else ""))

    rows = []
    channels = [c for c in core.getAvailableConfigs("Channel") if not a.channels or c in a.channels.split(",")]
    exposures = [float(e) for e in a.exposures.split(",")]
    print(f"{'channel':34s} " + " ".join(f"{e:>5g} ms: fps/max  render" for e in exposures))
    switch_profiles = {}
    for ch in channels:
        prof.take()
        switch = timed(lambda: (core.setConfig("Channel", ch), core.waitForSystem(), core.snapImage()))
        switch_profiles[ch] = prof.take()
        cells = []
        for e in exposures:
            core.setExposure(e)
            r = fps_run(core, a.seconds, prof)
            rows.append({"channel": ch, "exposureMs": e, "maxFps": 1000.0 / e, "switchMs": switch, **r})
            cells.append(f"{r['fps']:6.1f}/{1000 / e:<5.0f} {r['renderMs']:6.1f}" +
                         (f" [gap {r['worstGapMs']:.0f}, pf {r['prefetchMs']}]" if a.verbose else ""))
        print(f"{ch:34s} " + "  ".join(cells) + f"   (switch + snap {switch:.0f} ms)")

    core.setExposure(20.0)
    core.setConfig("Channel", channels[0])
    core.snapImage()
    lat, lat_profiles = {}, {}

    def action(name, fn):
        prof.take()
        lat[name] = timed(fn)
        lat_profiles[name] = prof.take()

    snaps = []
    for k in range(5):
        prof.take()
        snaps.append((timed(core.snapImage), prof.take()))
    snaps.sort(key=lambda s: s[0])
    lat["snap20ms"], lat_profiles["snap20ms"] = snaps[2]
    xy = core.getXYStageDevice()
    x0, y0 = core.getXYPosition(xy)
    action("stageMove30um", lambda: (core.setXYPosition(xy, x0 + 30.0, y0), core.waitForDevice(xy), core.snapImage()))
    objs = core.getAvailableConfigs("Objective")
    action("objectiveSwitch", lambda: (core.setConfig("Objective", objs[-1]), core.waitForSystem(), core.snapImage()))
    action("objectiveSwitchBack",
           lambda: (core.setConfig("Objective", objs[0]), core.waitForSystem(), core.snapImage()))
    z = core.getFocusDevice()
    action("zMove1um", lambda: (core.setPosition(z, core.getPosition(z) + 1.0), core.waitForDevice(z), core.snapImage()))
    print("latency (ms, call -> image, 20 ms exposure): " + ", ".join(f"{k} {v:.0f}" for k, v in lat.items()))
    gpu = core.getProperty("Renderer", "Test_GpuStatus") if core.hasProperty("Renderer", "Test_GpuStatus") else gpu
    core.unloadAllDevices()

    result = {"config": a.config, "fov": int(a.fov), "date": time.strftime("%Y-%m-%d"), "machine": machine(),
              "gpu": gpu, "loadMs": load_ms, "firstSnapMs": first_snap, "live": rows, "latencyMs": lat}
    if a.profile:
        result["profiles"] = {"startup": startup_profile, "channelSwitch": switch_profiles, "actions": lat_profiles}
    if a.json:
        Path(a.json).write_text(json.dumps(result, indent=1), encoding="utf-8")
    if a.history:
        add_to_history(Path(a.history), a.version, result)


def add_to_history(path, version, result):
    """benchmarks.json (a list of entries per version, tools/bench.py): this run as entry[version]["mm"]; older
    entries keep their numbers but drop their phase profiles (only the newest is shown in detail)."""
    hist = json.loads(path.read_text(encoding="utf-8")) if path.exists() else []
    for h in hist:
        if h.get("version") != version and isinstance(h.get("mm"), dict):
            h["mm"].pop("profiles", None)
            for r in h["mm"].get("live", []):
                r.pop("profile", None)
    entry = next((h for h in hist if h.get("version") == version), None)
    if entry is None:
        entry = {"version": version, "date": result["date"], "results": []}
        hist.append(entry)
    entry["mm"] = result
    path.write_text(json.dumps(hist, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
