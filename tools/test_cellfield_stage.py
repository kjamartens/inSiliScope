"""CellField pattern + XYStage checks (spec/PORT.md 10.5), with pymmcore-plus.

Exercises: the XY stage device (Busy while moving, move time ~ distance/speed,
position readback, MM's TransposeMirrorX flipping the direction), the camera's
CellField pattern rendering dyes of the insilicell world through the unchanged
render pipeline, a known feature shifting by the expected pixels between two
stage positions (live mode), and precomputed stacks that are byte-identical
after the stage went 1 mm away and came back.

Standalone (the Linux test build works too: tools/build_adapter_linux.sh):
    ADAPTER_DIR=<dir with the adapter> python tools/test_cellfield_stage.py
tools/test_insilicellscope.py also runs these checks at its end (run_checks).
Uses PSFParam_PsfModel=Gaussian, so no JVM is needed.
"""

import os
import sys
import time

import numpy as np


def _wait_idle(core, label, timeout_s=30.0):
    t0 = time.time()
    while core.deviceBusy(label):
        if time.time() - t0 > timeout_s:
            sys.exit(f"{label} still busy after {timeout_s} s")
        time.sleep(0.002)
    return time.time() - t0


def _wait_for_stack(core, cam, timeout_s=600.0):
    t0 = time.time()
    while not core.getProperty(cam, "General_StackGenerationStatus").startswith("Ready"):
        if time.time() - t0 > timeout_s:
            sys.exit("Timed out waiting for CellField stack generation")
        time.sleep(0.05)


def _live_sum(core, n):
    """Sum of n consecutive live frames (float)."""
    acc = None
    for _ in range(n):
        core.snapImage()
        img = core.getImage().astype(np.float64)
        acc = img if acc is None else acc + img
    return acc


def _shift(a, b):
    """Integer (dy, dx) such that b ~= a shifted by (dy, dx) (cross-correlation;
    not phase correlation, which would whiten towards any static pixel pattern)."""
    fa = np.fft.fft2(a - a.mean())
    fb = np.fft.fft2(b - b.mean())
    c = np.fft.ifft2(fb * np.conj(fa)).real
    c /= np.sqrt(((a - a.mean()) ** 2).sum() * ((b - b.mean()) ** 2).sum()) + 1e-12
    dy, dx = np.unravel_index(np.argmax(c), c.shape)
    h, w = c.shape
    return (dy - h if dy > h // 2 else dy), (dx - w if dx > w // 2 else dx), c.max()


def run_checks(core, cam="CFCam", xy="CFXY", z="CFZ"):
    core.loadDevice(cam, "inSiliCellScope", "Camera")
    core.setProperty(cam, "SimType_RandomSeed", "7")
    core.initializeDevice(cam)
    core.loadDevice(xy, "inSiliCellScope", "XYStage")
    core.initializeDevice(xy)
    if z not in core.getLoadedDevices():
        core.loadDevice(z, "inSiliCellScope", "ZStage")
        core.initializeDevice(z)
    core.setCameraDevice(cam)
    core.setXYStageDevice(xy)
    core.setPosition(z, 0.0)

    # ---- property surface -------------------------------------------------
    for p in ("SimType_CellFieldChunkSizeUm", "SimType_CellFieldOccupancy", "SimType_CellFieldPacking",
              "SimType_CellFieldCellDiameterMinUm", "SimType_CellFieldCellDiameterMaxUm",
              "SimType_CellFieldMicrotubuleDensityPerUm2", "SimType_CellFieldLabelingPct",
              "SimType_CellFieldFocusHeightUm", "SimType_CellFieldActivationMeanSec", "SimType_CellFieldZRangeUm"):
        assert core.hasProperty(cam, p), f"missing camera property {p}"
    for p in ("General_StageSpeedUmPerSec", "General_StageSettleMs", "General_StageLimitUm"):
        assert core.hasProperty(xy, p), f"missing XY stage property {p}"
    assert "CellField" in core.getAllowedPropertyValues(cam, "SimType_Pattern")
    print("CellField/XY stage properties present")

    # ---- XY stage motion --------------------------------------------------
    core.setXYPosition(xy, 0.0, 0.0)
    _wait_idle(core, xy)
    core.setProperty(xy, "General_StageSpeedUmPerSec", "1000")
    core.setProperty(xy, "General_StageSettleMs", "20")
    t0 = time.time()
    core.setXYPosition(xy, 300.0, 400.0)  # 500 um at 1000 um/s = 0.5 s (+ 20 ms settle)
    busy_right_after = core.deviceBusy(xy)
    mid = core.getXYPosition(xy)
    _wait_idle(core, xy)
    took = time.time() - t0
    pos = core.getXYPosition(xy)
    assert busy_right_after, "XY stage should report Busy right after a move starts"
    assert 0.45 < took < 1.0, f"500 um at 1000 um/s took {took:.3f} s, expected ~0.52 s"
    assert abs(pos[0] - 300) < 0.02 and abs(pos[1] - 400) < 0.02, f"arrived at {pos}"
    assert mid[0] < 300 and mid[1] < 400, f"position mid-move {mid} should be on the way"
    core.setProperty(xy, "General_StageSpeedUmPerSec", "100000")
    print(f"XY stage OK: 500 um move took {took:.3f} s at 1000 um/s, Busy during, arrived at {pos}")

    # ---- live mode: a feature moves by the stage step ----------------------
    core.setProperty(cam, "SimType_Pattern", "CellField")
    core.setProperty(cam, "PSFParam_PsfModel", "Gaussian")
    core.setProperty(cam, "General_FovSize", "128x128")
    core.setProperty(cam, "Background_BackgroundPhotonsPerSec", "0")
    core.setProperty(cam, "FluoParam_PhotonsPerSecond", "20000")
    # No static per-pixel pattern: it would correlate at zero shift.
    for p in ("CamParam_GainStdPctPerPixel", "CamParam_ReadNoiseStdPctPerPixel", "CamParam_OffsetStdADU"):
        core.setProperty(cam, p, "0")
    core.setProperty(cam, "SimType_CellFieldActivationMeanSec", "2")  # dense ON population: shows the MT network
    core.setProperty(cam, "General_AcqMode", "Live")
    core.setExposure(10.0)
    px_um = float(core.getProperty(cam, "General_PixelSizeNm")) / 1000.0

    # Find a FOV with structure: the brightest of a few spots.
    best = None
    for x in (0.0, 13.0, 26.0, 39.0):
        for y in (0.0, 13.0, 26.0):
            core.setXYPosition(xy, x, y)
            _wait_idle(core, xy)
            core.snapImage()  # a frame at the new pose
            s = _live_sum(core, 5).std()
            if best is None or s > best[0]:
                best = (s, x, y)
    s, x0, y0 = best
    assert s > 5.0, f"no structure found in any test FOV (best std {s:.2f}): CellField renders nothing?"
    core.setXYPosition(xy, x0, y0)
    _wait_idle(core, xy)
    core.snapImage()
    a = _live_sum(core, 40)
    step_um = 2.0
    core.setXYPosition(xy, x0 + step_um, y0)
    _wait_idle(core, xy)
    core.snapImage()
    b = _live_sum(core, 40)
    dy, dx, peak = _shift(a, b)
    expect = -round(step_um / px_um)
    assert (dy, dx) == (0, expect), f"stage +{step_um} um in x: image shift (dy, dx) = ({dy}, {dx}), expected (0, {expect})"
    print(f"Live CellField OK: stage +{step_um} um in x moved the structure by {dx} px (expected {expect}), "
          f"correlation peak {peak:.2f}")

    # MM's standard mirroring flips the direction.
    core.setProperty(xy, "TransposeMirrorX", "1")
    ux, uy = core.getXYPosition(xy)
    core.setXYPosition(xy, ux + step_um, uy)
    _wait_idle(core, xy)
    core.snapImage()
    c = _live_sum(core, 40)
    dy2, dx2, _ = _shift(b, c)
    core.setProperty(xy, "TransposeMirrorX", "0")
    assert (dy2, dx2) == (0, -expect), f"with TransposeMirrorX=1, +{step_um} um moved the image by ({dy2}, {dx2})"
    print(f"TransposeMirrorX OK: the same user-coordinate step now moves the image by {dx2} px")

    # ---- precomputed: stage 1 mm away and back, identical stack -----------
    core.setProperty(cam, "SimType_CellFieldActivationMeanSec", "100")
    core.setProperty(cam, "General_AcqMode", "Precomputed")
    core.setExposure(20.0)

    def stack_frames_at(x, y, n=5):
        core.setXYPosition(xy, x, y)
        _wait_idle(core, xy)
        core.setProperty(cam, "General_GenerateStack", "1")
        _wait_for_stack(core, cam)
        frames = []
        for _ in range(n):
            core.snapImage()
            frames.append(core.getImage().copy())
        return frames

    t0 = time.time()
    first = stack_frames_at(x0, y0)
    gen_s = time.time() - t0
    stack_frames_at(x0 + 1000.0, y0)
    back = stack_frames_at(x0, y0)
    assert all(np.array_equal(f, g) for f, g in zip(first, back)), \
        "precomputed CellField stack differs after the stage went 1 mm away and back"
    assert any(f.std() > 3 for f in first), "precomputed CellField frames look empty"
    print(f"Precomputed CellField OK: 1000-frame stack in {gen_s:.1f} s, byte-identical after a 1 mm excursion")

    # SimType_CellFieldZRangeUm is its own setting: a thin slab renders fewer
    # dyes than the default 7 um one, 0 (no z limit) at least as many.
    def mean_signal(zr):
        core.setProperty(cam, "SimType_CellFieldZRangeUm", str(zr))
        return float(np.mean([f.astype(np.float64).mean() for f in stack_frames_at(x0, y0, n=20)]))
    thin, default, unlimited = mean_signal(0.2), mean_signal(7), mean_signal(0)
    core.setProperty(cam, "SimType_CellFieldZRangeUm", "7")
    assert thin < default <= unlimited + 1e-9, f"z range 0.2/7/0 um: mean {thin:.3f}/{default:.3f}/{unlimited:.3f} ADU"
    print(f"CellFieldZRangeUm OK: mean frame {thin:.2f} (0.2 um) < {default:.2f} (7 um) <= {unlimited:.2f} ADU (no limit)")

    core.setProperty(cam, "General_AcqMode", "Live")
    core.setXYPosition(xy, 0.0, 0.0)
    _wait_idle(core, xy)


if __name__ == "__main__":
    from pymmcore_plus import CMMCorePlus

    core = CMMCorePlus()
    dirs = [d for d in (os.environ.get("ADAPTER_DIR"), os.environ.get("MM_DIR")) if d]
    if not dirs:
        sys.exit("Set ADAPTER_DIR to the directory holding the inSiliCellScope adapter.")
    core.setDeviceAdapterSearchPaths(dirs)
    run_checks(core)
    print("All CellField / XY stage checks passed.")
