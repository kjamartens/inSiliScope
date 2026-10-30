"""Smoke test of the Fiducials device in Micro-Manager (pymmcore-plus): beads on the sample.

    ADAPTER_DIR=<dir with mmgr_dal_inSiliScope.dll> python tools/test_fiducials.py

Live frames at 128 px, both cell-field targets unlabelled (only the beads emit), the 561 nm line alone; spec/ALGORITHM.md
"Fiducial beads", spec/MM_DEVICES.md.
- Layout: the Fiducials device and its rows; off by default (DensityPerUm2 0): the frames hold noise only.
- TetraSpeck beads (DyeCount 30: bright, not saturating) at 0.05 /um^2 make bright spots that stay where they are.
- Live bleaching from the light history (BleachHalfTimeSec 0.1 at the peak; only the dark-red dye passes this light
  path, excited off its peak at 561 nm: ~1 s here): over a 2 s sequence the beads fade; a snap after a
  pause (TimeWhileIdle Paused) continues where the sequence ended; a bead setting that rebuilds the beads (DiameterCvPct)
  keeps the illumination history (the beads stay faded).
- Height OnTop puts the beads on the cells: other frames than on the coverslip.
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import load_scope, search_paths, timed_prints  # noqa: E402

LASERS = (405, 488, 561, 640, 730)


def sequence(core, n):
    """n frames of a live sequence acquisition, as float arrays."""
    core.startSequenceAcquisition(n, 0, True)
    frames, t0 = [], time.time()
    while len(frames) < n:
        if core.getRemainingImageCount() > 0:
            frames.append(core.popNextImage().astype(np.float64))
        elif time.time() - t0 > 120:
            sys.exit("sequence timed out")
        else:
            time.sleep(0.002)
    core.stopSequenceAcquisition()
    while core.isSequenceRunning():
        time.sleep(0.002)
    return frames


def snap(core):
    core.snapImage()
    return core.getImage().astype(np.float64)


def peaks(img, dark, k=8.0):
    """Local maxima more than k robust sigmas above the dark level, 8 px from the edge and apart."""
    d = img - dark
    sig = 1.4826 * float(np.median(np.abs(d - np.median(d)))) + 1e-9
    out = []
    for y, x in zip(*np.nonzero(d > k * sig)):
        if 8 <= x < d.shape[1] - 8 and 8 <= y < d.shape[0] - 8 and d[y, x] == d[y - 3:y + 4, x - 3:x + 4].max():
            if all((x - a) ** 2 + (y - b) ** 2 > 64 for a, b in out):
                out.append((x, y))
    return out


def bead_signal(img, dark, at, r=4):
    return float(sum((img[y - r:y + r + 1, x - r:x + r + 1] - dark).sum() for x, y in at))


def main():
    from pymmcore_plus import CMMCorePlus

    timed_prints()
    t_start = time.time()
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths())
    load_scope(core, seed=7, fov="128x128")   # TimeWhileIdle Paused, the lasers open, autoshutter off
    core.setExposure(50)
    assert "Fiducials" in core.getLoadedDevices(), "the Fiducials device is not loaded"
    for p, v in (("Type", "Gold"), ("DensityPerUm2", 0), ("DiameterNm", 100), ("DiameterCvPct", 12),
                 ("Height", "OnTop"), ("LayerUm", 10), ("BleachHalfTimeSec", 288), ("DyeCount", -1),
                 ("BlockingOD", 8)):
        got = core.getProperty("Fiducials", p)
        assert (float(got) == v) if isinstance(v, (int, float)) else got == v, f"Fiducials.{p} = {got}, not {v}"
    print("Layout OK: Fiducials rows and defaults")

    for t in ("Microtubules", "NuclearPores"):
        core.setProperty("CellField", f"{t}_Label", "None")
    for nm in LASERS:
        core.setProperty("Lasers", f"Laser{nm}KWcm2", "1" if nm == 561 else "0")
    core.setXYPosition("XYStage", 0.0, 0.0)
    core.waitForDevice("XYStage")
    core.setProperty("Lasers", "State", "0")
    dark = float(np.mean([f.mean() for f in sequence(core, 3)]))
    core.setProperty("Lasers", "State", "1")
    off = snap(core)
    assert not peaks(off, dark), f"beads off, yet {len(peaks(off, dark))} spots"
    print(f"Off: no spots (dark level {dark:.1f} ADU)")

    core.setProperty("Fiducials", "Type", "TetraSpeck")
    core.setProperty("Fiducials", "Height", "Coverslip")   # in focus, as sharp spots
    core.setProperty("Fiducials", "DyeCount", "30")
    core.setProperty("Fiducials", "DensityPerUm2", "0.05")
    core.setProperty("Fiducials", "BleachHalfTimeSec", "0.1")
    core.setXYPosition("XYStage", 80.0, 40.0)   # a place never lit
    core.waitForDevice("XYStage")
    first = snap(core)
    at = peaks(first, dark)
    assert len(at) >= 3, f"TetraSpeck at 0.05 /um^2: {len(at)} spots"
    assert first.max() < 60000, f"saturated beads ({first.max():.0f} ADU)"
    s0 = bead_signal(first, dark, at)
    print(f"Beads: {len(at)} spots, peak {first.max() - dark:.0f} ADU above dark")

    frames = sequence(core, 40)
    s = [bead_signal(f, dark, at) for f in frames]
    assert s[-1] < 0.6 * s0 and s[-1] < s[0], f"no live bleaching: {s0:.0f} -> {s[0]:.0f} .. {s[-1]:.0f}"
    time.sleep(1.0)   # paused: no light, no bleaching
    after = bead_signal(snap(core), dark, at)
    assert 0.85 * s[-1] < after < 1.15 * s[-1], f"after a pause the beads should stay faded: {s[-1]:.0f} -> {after:.0f}"
    core.setProperty("Fiducials", "DiameterCvPct", "13")   # the beads rebuilt, the history kept
    kept = bead_signal(snap(core), dark, at)
    assert kept < 0.6 * s0, f"a bead setting should keep the illumination history: {s0:.0f} -> {kept:.0f}"
    print(f"Live bleaching: {s0:.0f} -> {s[-1]:.0f} over 2 s ({s[-1] / s0:.2f}); after a pause {after:.0f}, "
          f"after DiameterCvPct {kept:.0f}")

    core.setProperty("Fiducials", "DiameterCvPct", "12")
    core.setProperty("Fiducials", "BleachHalfTimeSec", "288")
    core.setXYPosition("XYStage", 40.0, 40.0)
    core.waitForDevice("XYStage")
    glass = snap(core)
    core.setProperty("Fiducials", "Height", "OnTop")
    top = snap(core)
    d = float(np.abs(top - glass).mean())
    assert d > 1.0, f"OnTop should change the frames (mean |diff| {d:.2f} ADU)"
    print(f"Height OnTop: mean |diff| {d:.1f} ADU against Coverslip")
    print(f"Fiducials: PASS ({time.time() - t_start:.0f} s)")


if __name__ == "__main__":
    main()
