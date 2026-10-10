"""Smoke test of the sample's history in Micro-Manager (pymmcore-plus): a change acts from now on.

    ADAPTER_DIR=<dir with mmgr_dal_inSiliScope.dll> python tools/test_history.py

Live sequence acquisitions at 128 px, Gaussian PSF, each check on a fresh place of the sample (spec/ALGORITHM.md
"Rate history"; docs/physics/photophysics.md "Illumination history"). The signal of a frame is its mean above the
dark frames' mean.
- WideField (mEGFP, 0.002 kW/cm^2 so the camera does not clip, a small photon budget): x4 power -> the first frame
  after is ~4x the last one before (the dyes bleached so far stay bleached; re-reading the past at the new power
  would give far less).
- PALM (mEos3.2): the 405 line off -> the activated dyes blink on (the first frames after keep > 50% of the signal)
  and then die out (no new activations).
- Stop/start: a second sequence continues where the first stopped.
- SampleHolder.TimeWhileIdle: Paused -> 2 s idle with the lasers open changes nothing; Running -> they bleach the
  sample meanwhile; lasers at 0 kW/cm^2 bleach nothing even then.
- Drift: Paused -> 2 s idle moves the sample by at most a frame's step; Running -> by ~speed x 2 s (directed drift,
  Camera.Test_DriftNm).
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import custom_microtubule_dye, label, load_scope, search_paths, timed_prints  # noqa: E402

LASERS = (405, 488, 561, 640, 730)


def lasers(core, **kw):
    """Every laser line off but the ones given (kW/cm^2), e.g. lasers(core, l488=1)."""
    for nm in LASERS:
        core.setProperty("Lasers", f"Laser{nm}KWcm2", str(kw.get(f"l{nm}", 0)))


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


def drift_nm(core):
    return [float(v) for v in core.getProperty("Camera", "Test_DriftNm").split()]


def main():
    from pymmcore_plus import CMMCorePlus

    timed_prints()
    t_start = time.time()
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths())
    load_scope(core, seed=7, fov="128x128")   # TimeWhileIdle Paused, the lasers open (explicitly), autoshutter off
    core.setProperty("Renderer", "PsfModel", "Gaussian")
    core.setExposure(50)
    assert core.getProperty("SampleHolder", "TimeWhileIdle") == "Paused"
    # Places with cells (seed 7), 40 um apart: each check starts on a sample never lit.
    places = iter([(0.0, 0.0), (80.0, 40.0), (40.0, 40.0), (120.0, 0.0), (160.0, 0.0), (160.0, 80.0), (40.0, 80.0)])

    def fresh_place():
        x, y = next(places)
        core.setXYPosition("XYStage", x, y)
        core.waitForDevice("XYStage")
        return x, y

    # The dark level: the lasers' shutter closed.
    core.setProperty("Lasers", "State", "0")
    dark = float(np.mean([f.mean() for f in sequence(core, 5)]))
    core.setProperty("Lasers", "State", "1")
    sig = lambda frames: [f.mean() - dark for f in frames]  # noqa: E731
    print(f"dark level {dark:.2f} ADU")

    # ---- WideField: x4 power ----
    P = 0.002
    label(core, "WideField", "mEGFP")
    lasers(core, l488=P)
    det = float(core.getProperty("Fluorophores", "Microtubules_PhotonsPerSecOn"))
    pct = float(core.getProperty("Fluorophores", "Microtubules_DetectedPct"))
    emission = det / (pct / 100.0)
    custom_microtubule_dye(core, PhotonBudget=f"{emission * 1.5:.6g}")   # bleaches in ~1 s at P
    lasers(core, l488=P)   # (a dye pick applies its light preset)
    fresh_place()
    before = sig(sequence(core, 20))
    lasers(core, l488=4 * P)
    after = sig(sequence(core, 5))
    decay, ratio = before[-1] / before[0], after[0] / before[-1]
    print(f"WideField: 20 frames at {P:g} kW/cm^2 decay to {decay:.2f}; at x4 the first frame is {ratio:.2f}x the "
          f"last (expected ~4)")
    assert before[0] > 5, f"WideField: too little signal ({before[0]:.2f} ADU)"
    assert decay < 0.8, f"WideField: the sample should bleach visibly in 1 s (decay {decay:.2f})"
    assert 3.0 < ratio < 4.8, f"WideField: x4 power should give ~4x the last frame, got {ratio:.2f}"

    # ---- Stop / start: the next sequence continues ----
    lasers(core, l488=P)
    s1 = sig(sequence(core, 5))
    s2 = sig(sequence(core, 5))
    cont = s2[0] / s1[-1]
    print(f"Stop/start: the next sequence's first frame is {cont:.2f}x the last one (expected ~0.97)")
    assert 0.85 < cont < 1.1, f"stop/start: the sample should continue, got {cont:.2f}"

    # ---- TimeWhileIdle: idle light ----
    fresh_place()
    a = sig(sequence(core, 5))
    time.sleep(2.0)
    b = sig(sequence(core, 5))
    paused = b[0] / a[-1]
    core.setProperty("SampleHolder", "TimeWhileIdle", "Running")
    lasers(core, l488=0)
    time.sleep(2.0)   # running, the shutter open, no light reaches the sample
    lasers(core, l488=P)
    c = sig(sequence(core, 5))
    dark_idle = c[0] / b[-1]
    time.sleep(2.0)   # running, lit
    d = sig(sequence(core, 5))
    running = d[0] / c[-1]
    core.setProperty("SampleHolder", "TimeWhileIdle", "Paused")
    print(f"Idle 2 s: Paused {paused:.2f}x, Running at 0 kW/cm^2 {dark_idle:.2f}x, Running lit {running:.2f}x "
          f"(expected ~1, ~1, < 0.6)")
    assert 0.85 < paused < 1.1, f"Paused: 2 s idle should change nothing, got {paused:.2f}"
    assert 0.85 < dark_idle < 1.1, f"Running with the lasers at 0: nothing should bleach, got {dark_idle:.2f}"
    assert running < 0.6, f"Running: 2 s idle with the lasers open should bleach, got {running:.2f}"

    # ---- PALM: the 405 line off ----
    label(core, "PALM", "mEos3.2")
    l405 = float(core.getProperty("Lasers", "Laser405KWcm2"))
    l561 = float(core.getProperty("Lasers", "Laser561KWcm2"))
    print(f"PALM preset: 405 at {l405:g}, 561 at {l561:g} kW/cm^2")
    fresh_place()
    lasers(core, l405=max(l405, 0.01) * 10, l561=l561)   # many dyes activated and blinking
    on = sig(sequence(core, 40))
    lasers(core, l405=0, l561=l561)
    off = sig(sequence(core, 40))
    level = float(np.mean(on[-5:]))
    first, last = float(np.mean(off[:3])), float(np.mean(off[-5:]))
    print(f"PALM: 405 on {level:.2f} ADU; 405 off: the first frames {first:.2f}, 2 s later {last:.2f}")
    assert level > 0.5, f"PALM: too little signal ({level:.2f} ADU)"
    assert first > 0.5 * level, f"PALM: the activated dyes should keep blinking after the 405 goes off ({first:.2f})"
    assert last < 0.6 * first, f"PALM: without the 405 the blinking should die out ({first:.2f} -> {last:.2f})"
    lasers(core, l405=l405, l561=l561)

    # ---- Drift between acquisitions ----
    label(core, "WideField", "mEGFP")
    lasers(core, l488=P)
    for p, v in (("DriftXyAngleDeg", "0"), ("DriftXyAngleWanderDeg", "0"), ("DriftSpeedWanderPct", "0"),
                 ("DriftXySpeedNmPerSec", "250")):
        core.setProperty("SampleHolder", p, v)
    core.snapImage()
    x0 = drift_nm(core)[0]
    time.sleep(2.0)
    core.snapImage()
    x1 = drift_nm(core)[0]
    core.setProperty("SampleHolder", "TimeWhileIdle", "Running")
    core.snapImage()
    x2 = drift_nm(core)[0]
    time.sleep(2.0)
    core.snapImage()
    x3 = drift_nm(core)[0]
    core.setProperty("SampleHolder", "TimeWhileIdle", "Paused")
    print(f"Drift at 250 nm/s over 2 s idle: Paused {x1 - x0:.0f} nm, Running {x3 - x2:.0f} nm (expected ~12, ~500)")
    assert 0 <= x1 - x0 < 40, f"Paused: the sample should not drift while idle ({x1 - x0:.0f} nm)"
    assert 350 < x3 - x2 < 700, f"Running: the sample should drift ~500 nm in 2 s idle ({x3 - x2:.0f} nm)"
    core.setProperty("SampleHolder", "DriftXySpeedNmPerSec", "0")

    core.unloadAllDevices()
    print(f"test_history: all checks passed in {time.time() - t_start:.0f} s")


if __name__ == "__main__":
    main()
