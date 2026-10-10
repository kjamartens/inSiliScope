"""Loads the shipped Micro-Manager configurations (adapter/inSiliScope/config/, tools/gen_mm_configs.py) and checks them:
the startup state, every preset of every group (applies, and MM recognises it as the current one), MM's pixel size =
the engine's for every objective x camera, and a snap per Channel preset that is not blank.

  ADAPTER_DIR=<dll dir> python tools/test_mm_configs.py      (< 2 min)
"""

import os
import sys
import time
from pathlib import Path

os.environ["ISC_TEST"] = "0"   # what a user loads: no Test rows
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402
from gen_mm_configs import OUT, SENSOR_UM, TIERS, load_sensors  # noqa: E402
from isc_mm import search_paths  # noqa: E402


def check(tier):
    from pymmcore_plus import CMMCorePlus

    t0 = time.time()
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths())
    core.loadSystemConfiguration(str(OUT / f"inSiliScope_{tier}.cfg"))
    assert core.getProperty("Hub", "Detail") == tier
    assert not any(p.startswith("Test_") for p in core.getDevicePropertyNames("Camera")), "Test rows without ISC_TEST"
    assert core.getChannelGroup() == "Channel" and core.getAutoShutter()
    start_channel = core.getCurrentConfig("Channel")
    assert start_channel == "ATTO655 DNA-PAINT", f"{tier}: startup channel {start_channel!r}"

    def objective_mag():   # the label's (Basic has no Magnification readout): 100x/1.40 Oil -> 100
        return float(core.getProperty("Objective", "Label").split("x/")[0])

    def expected_px_um():
        cam = core.getProperty("Camera", "CameraPreset")
        return SENSOR_UM[cam] / (objective_mag() * float(core.getProperty("EmissionPath", "EmissionMagnification")))

    assert abs(core.getPixelSizeUm() - 6.5 / (100 * 0.667)) < 1e-6, f"{tier}: startup pixel {core.getPixelSizeUm()}"

    # Every preset applies and is then the group's current preset.
    groups = [g for g in core.getAvailableConfigGroups() if g != "System"]
    for g in groups:
        for preset in core.getAvailableConfigs(g):
            core.setConfig(g, preset)
            core.waitForSystem()
            cur = core.getCurrentConfig(g)
            assert cur == preset, f"{tier}: {g} = {preset!r} applied, MM reports {cur!r}"
    n_presets = sum(len(core.getAvailableConfigs(g)) for g in groups)

    # MM's pixel size (config / EmissionPath) = the engine's, for every objective x camera.
    n_px = 0
    for cam in core.getAvailableConfigs("Camera"):
        core.setConfig("Camera", cam)
        for obj in core.getAvailableConfigs("Objective"):
            core.setConfig("Objective", obj)
            want = expected_px_um()
            got = core.getPixelSizeUm()
            assert got > 0 and abs(got - want) < 1e-9 * max(1.0, want) + 1e-9, \
                f"{tier}: {obj} + {cam}: MM pixel {got} um, engine {want} um"
            if tier != "Basic":
                eng = float(core.getProperty("Camera", "PixelSizeNm")) / 1000.0
                assert abs(eng - got) < 1e-6, f"{tier}: {obj} + {cam}: Camera PixelSizeNm {eng} um vs MM {got} um"
            n_px += 1
    core.setConfig("Camera", "Kinetix22")
    core.setConfig("Objective", "100x NA 1.40 Oil")
    core.setConfig("Quality", "Realistic")

    # A snap per Channel preset is not blank (the autoshutter opens the preset's shutter).
    offset = 100.0
    lines = []
    for ch in core.getAvailableConfigs("Channel"):
        core.setConfig("Channel", ch)
        core.waitForSystem()
        core.snapImage()
        img = core.getImage().astype(np.float64)
        med, top, mean = float(np.median(img)), float(img.max()), float(img.mean())
        if "BrightField" in ch:
            ok = mean > offset + 50
        else:
            ok = top - med > 20
        assert ok, f"{tier}: Channel {ch!r} snap looks blank (mean {mean:.1f}, median {med:.1f}, max {top:.0f})"
        assert core.getProperty("Lasers", "State") == "0" and core.getProperty("TransmittedLamp", "State") == "0", \
            f"{tier}: the autoshutter should close the shutters after a snap"
        lines.append(f"{ch}: mean {mean:.0f}, max {top:.0f}")
    core.unloadAllDevices()
    print(f"[{time.time() - t0:5.1f} s] {tier}: {len(groups)} groups, {n_presets} presets, {n_px} pixel sizes; "
          + "; ".join(lines))


if __name__ == "__main__":
    if not os.environ.get("ADAPTER_DIR") and not os.environ.get("MM_DIR"):
        sys.exit("Set ADAPTER_DIR to the directory holding the inSiliScope adapter.")
    load_sensors()
    t_start = time.time()
    for tier in TIERS:
        check(tier)
    print(f"All Micro-Manager config checks passed ({time.time() - t_start:.0f} s).")
