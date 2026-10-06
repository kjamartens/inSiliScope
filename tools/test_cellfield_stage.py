"""CellField pattern + XYStage checks (spec/PORT.md 10.5), with pymmcore-plus.

Exercises: the XY stage device (Busy while moving, move time ~ distance/speed,
position readback, MM's TransposeMirrorX flipping the direction), the camera's
CellField pattern rendering dyes of the insiliscope world through the unchanged
render pipeline, a known feature shifting by the expected pixels between two
stage positions (live mode), precomputed stacks that are byte-identical
after the stage went 1 mm away and came back, and the WideField modality
(bleaching half time, split labelling, a world-anchored bleach map in live mode),
a hardware z stack (the ZStage's sequence, one position per camera frame),
and the BrightField modality (lamp flux, defocus contrast, live = precomputed,
z sequence).

Standalone (the Linux test build works too: tools/build_adapter_linux.sh):
    ADAPTER_DIR=<dir with the adapter> python tools/test_cellfield_stage.py [--only drift,wfzseq]
Each printed line starts with the seconds since the previous one.
tools/test_insiliscope.py also runs these checks at its end (run_checks).
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


def _ls_shift(a, b, r=24):
    """Integer (dy, dx) with b[y, x] ~= a[y - dy, x - dx]: the least mean squared difference over the overlap. Unlike
    the circular cross-correlation (_shift) it also finds the shift of smooth, featureless images."""
    h, w = a.shape
    best = None
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            pa = a[max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)]
            pb = b[max(0, dy):h - max(0, -dy), max(0, dx):w - max(0, -dx)]
            e = float(np.mean((pa - pb) ** 2))
            if best is None or e < best[0]:
                best = (e, dy, dx)
    return best[1], best[2], best[0]


def _drift_nm(seed, frames, frame_sec, sxy, sz):
    """Simulation/Drift.cpp in Python: the sample drift (x, y, z nm) of each frame."""
    m = 0xFFFFFFFF

    def pcg4d(a, b, c, d):
        a, b, c, d = [(v * 1664525 + 1013904223) & m for v in (a, b, c, d)]
        a = (a + b * d) & m; b = (b + c * a) & m; c = (c + a * b) & m; d = (d + b * c) & m
        a ^= a >> 16; b ^= b >> 16; c ^= c >> 16; d ^= d >> 16
        a = (a + b * d) & m; b = (b + c * a) & m; c = (c + a * b) & m; d = (d + b * c) & m
        return [a, b, c, d]

    s32 = (seed ^ 0x44524654) & m
    out, x, y, zz = [(0.0, 0.0, 0.0)], 0.0, 0.0, 0.0
    for f in range(1, frames):
        u = [((w >> 9) + 0.5) * 1.1920928955078125e-7 for w in pcg4d(s32, f, 0, 0) + pcg4d(s32, f, 0, 1)]
        g = [np.sqrt(-2.0 * np.log(u[2 * k])) * np.cos(6.283185307179586 * u[2 * k + 1]) for k in range(3)]
        r = np.sqrt(frame_sec)
        x += sxy * r * g[0]; y += sxy * r * g[1]; zz += sz * r * g[2]
        out.append((x, y, zz))
    return out


def _drift_checks(core, cam, xy, x0, y0):
    """Random-walk sample drift (SimType_DriftXyNmPerSqrtSec / ZNmPerSqrtSec): the properties are there (the linear
    SimType_DriftNmPerSec is gone) and wired into every modality. The physics (exact sub-pixel shift, focus grid,
    C++ = JS) is ctest drift / widefield / brightfield and scope_parity.
    - Precomputed, each modality: frame 9 of a drifting 10-frame stack is the still stack's frame 9 moved by the
      seed's drift path, the path the cli and viewer take (_drift_nm).
    - Directed (SuperRes stack): a constant velocity at a set angle moves frame 9 by exactly v t.
    - Live, WideField and BrightField (SuperRes blinks differ every frame; its live drift is the same per-emitter
      offset as its stacks): frame 9 of a sequence acquisition moves by the same path as the stacks' frame 9."""
    assert not core.hasProperty(cam, "SimType_DriftNmPerSec"), "SimType_DriftNmPerSec should be gone"
    for p, v in (("SimType_DriftXyNmPerSqrtSec", 0.0), ("SimType_DriftZNmPerSqrtSec", 0.0),
                 ("SimType_DriftXySpeedNmPerSec", 0.0), ("SimType_DriftZSpeedNmPerSec", 0.0),
                 ("SimType_DriftXyAngleDeg", -1.0), ("SimType_DriftXyAngleWanderDeg", 0.0),
                 ("SimType_DriftSpeedWanderPct", 0.0), ("SimType_DriftWanderTimeSec", 60.0)):
        assert core.hasProperty(cam, p) and float(core.getProperty(cam, p)) == v, f"{p} missing or not {v}"
    core.setXYPosition(xy, x0, y0)  # the field with structure
    _wait_idle(core, xy)
    seed = int(core.getProperty(cam, "SimType_RandomSeed"))
    px = float(core.getProperty(cam, "General_PixelSizeNm"))
    # 1000 nm/sqrt(s) (the property's maximum) and 200 ms frames: frame 9 (1.8 s) sits ~1.3 um (~13 px rms per axis)
    # from frame 0. A snap of a precomputed stack takes its exposure: 10 snaps = 2 s per stack.
    sxy, sz, exp_ms, k = 1000.0, 30.0, 200.0, 9
    path = _drift_nm(seed, k + 1, exp_ms / 1000.0, sxy, sz)
    expect = (round(path[k][1] / px), round(path[k][0] / px))

    def drift(on):
        core.setProperty(cam, "SimType_DriftXyNmPerSqrtSec", str(sxy if on else 0))
        core.setProperty(cam, "SimType_DriftZNmPerSqrtSec", str(sz if on else 0))

    def stack_frame():
        core.setProperty(cam, "General_GenerateStack", "1")
        _wait_for_stack(core, cam)
        for _ in range(k + 1):
            core.snapImage()
        return core.getImage().astype(np.float64)

    def setup(modality):
        core.setProperty(cam, "General_ImagingModality", modality)
        if modality == "BrightField":
            # Single frames: unstained cells (~0.5% contrast) sit below the shot noise, so the cells absorb here
            # (and no static pixel pattern pulls the comparison to zero shift); 16000 photons/px, no saturation.
            core.setProperty(cam, "General_BrightFieldQuality", "1")
            core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", str(16000.0 / (exp_ms / 1000.0)))
            core.setProperty(cam, "SimType_CellFieldAbsorptionPerUm", "0.3")
            core.setProperty(cam, "CamParam_GainStdPctPerPixel", "0")

    core.setProperty(cam, "General_StackLength", str(k + 1))
    core.setProperty(cam, "General_AcqMode", "Precomputed")
    core.setExposure(exp_ms)
    for modality in ("SuperRes", "WideField", "BrightField"):
        setup(modality)
        drift(False)
        still = stack_frame()
        drift(True)
        moved = stack_frame()
        dy, dx, e = _ls_shift(still, moved)
        assert abs(dy - expect[0]) <= 1 and abs(dx - expect[1]) <= 1,             f"{modality}: stack frame {k} moved by ({dy}, {dx}) px, the drift path says {expect}"
        print(f"Drift OK ({modality}, precomputed): frame {k} moved by ({dy}, {dx}) px (path {expect})")

    # Directed drift (SuperRes stack): 800 nm/s at 30 deg, no jitter: frame 9 (1.8 s) moved by 1.44 um along it.
    setup("SuperRes")
    drift(False)
    still = stack_frame()
    core.setProperty(cam, "SimType_DriftXySpeedNmPerSec", "800")
    core.setProperty(cam, "SimType_DriftXyAngleDeg", "30")
    moved = stack_frame()
    core.setProperty(cam, "SimType_DriftXySpeedNmPerSec", "0")
    core.setProperty(cam, "SimType_DriftXyAngleDeg", "-1")
    d_nm = 800.0 * k * exp_ms / 1000.0
    expect_dir = (round(d_nm * np.sin(np.pi / 6) / px), round(d_nm * np.cos(np.pi / 6) / px))
    dy, dx, e = _ls_shift(still, moved)
    assert abs(dy - expect_dir[0]) <= 1 and abs(dx - expect_dir[1]) <= 1,         f"directed drift: stack frame {k} moved by ({dy}, {dx}) px, expected {expect_dir}"
    print(f"Drift OK (directed, precomputed): 800 nm/s at 30 deg moved frame {k} by ({dy}, {dx}) px (expected {expect_dir})")

    # Live, WideField and BrightField: a 10-frame sequence acquisition at 200 ms restarts the drift at its first frame,
    # so its frame 9 must move by the same seed path as the stacks' frame 9 (live = precomputed). Non-bleaching dyes,
    # so WideField does not dim meanwhile.
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "0")
    core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "70")
    core.setProperty(cam, "General_AcqMode", "Live")
    for modality in ("WideField", "BrightField"):
        setup(modality)
        drift(True)
        time.sleep(1.0)  # the live scene is built
        core.startSequenceAcquisition(k + 1, 0, True)
        frames, t0 = [], time.time()
        while len(frames) < k + 1:
            if core.getRemainingImageCount() > 0:
                frames.append(core.popNextImage().astype(np.float64))
            elif time.time() - t0 > 120:
                sys.exit(f"{modality} live drift: sequence timed out")
            else:
                time.sleep(0.005)
        core.stopSequenceAcquisition()
        dy, dx, e = _ls_shift(frames[0], frames[k])
        assert abs(dy - expect[0]) <= 1 and abs(dx - expect[1]) <= 1,             f"{modality} live: frame {k} of a sequence moved by ({dy}, {dx}) px, the drift path says {expect}"
        print(f"Drift OK ({modality}, live): sequence frame {k} moved by ({dy}, {dx}) px (path {expect})")

    drift(False)
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "10")
    core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "0")
    core.setProperty(cam, "SimType_CellFieldAbsorptionPerUm", "0")
    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "80000")
    core.setProperty(cam, "General_BrightFieldQuality", "3")
    core.setProperty(cam, "General_StackLength", "300")
    core.setProperty(cam, "General_ImagingModality", "SuperRes")
    core.setExposure(20.0)


SECTIONS = ("props", "xy", "live", "precomputed", "zrange", "zsign", "bleach", "widefield", "wfzseq", "brightfield",
            "bfzseq", "drift")


def run_checks(core, cam="CFCam", xy="CFXY", z="CFZ", only=None):
    """Every section, or only those named in `only` (the setup and the search for a FOV with
    structure always run; each section sets up what it needs from there)."""
    want = lambda name: only is None or name in only
    core.loadDevice(cam, "inSiliScope", "Camera")
    core.setProperty(cam, "SimType_RandomSeed", "7")
    core.initializeDevice(cam)
    core.loadDevice(xy, "inSiliScope", "XYStage")
    core.initializeDevice(xy)
    if z not in core.getLoadedDevices():
        core.loadDevice(z, "inSiliScope", "ZStage")
        core.initializeDevice(z)
        assert abs(core.getPosition(z) - 0.5) < 1e-9, f"ZStage should start at 0.5 um, got {core.getPosition(z)}"
        print("ZStage starts at 0.5 um")
    core.setCameraDevice(cam)
    core.setXYStageDevice(xy)
    # 300-frame stacks (the most any check reads; the default is 1000): a stack costs its length.
    core.setProperty(cam, "General_StackLength", "300")
    core.setPosition(z, 1.5)  # focal plane 1.5 um above the coverslip (was the old default view)

    if want("props"):
        # ---- property surface -------------------------------------------------
        for p in ("SimType_CellFieldChunkSizeUm", "SimType_CellFieldOccupancy", "SimType_CellFieldPacking",
                  "SimType_CellFieldCellDiameterMinUm", "SimType_CellFieldCellDiameterMaxUm",
                  "SimType_CellFieldMicrotubuleDensityPerUm2", "SimType_CellFieldLabelingPctBleaching",
                  "SimType_CellFieldFocusHeightUm", "SimType_CellFieldMilliActivationRatePerDyePerSec", "SimType_CellFieldZRangeUm",
                  "SimType_CellFieldLabelingPctNonBleaching"):
            assert core.hasProperty(cam, p), f"missing camera property {p}"
        # Nucleus shape and microtubule start/end (2026-10-05): the core's defaults.
        for p, v in (("SimType_CellFieldNucBaseMinUm", 0.4), ("SimType_CellFieldNucBaseMaxUm", 0.9),
                     ("SimType_CellFieldNucIrregMin", 0.03), ("SimType_CellFieldNucIrregMax", 0.2),
                     ("SimType_CellFieldNucBendMin", 0.0), ("SimType_CellFieldNucBendMax", 0.3),
                     ("SimType_CellFieldNucSmooth", 2.5), ("SimType_CellFieldNucThickIrreg", 0.1),
                     ("SimType_CellFieldNucAsym", 0.5), ("SimType_CellFieldNucWidestMin", 0.2),
                     ("SimType_CellFieldNucWidestMax", 0.4), ("SimType_CellFieldMicrotubuleStartDecayPct", 1.6),
                     ("SimType_CellFieldMicrotubuleEndDecayPct", 20.0), ("SimType_CellFieldMicrotubuleDirKappa", 1.5)):
            assert core.hasProperty(cam, p), f"missing camera property {p}"
            got = float(core.getProperty(cam, p))
            assert abs(got - v) < 1e-9, f"{p} default {got}, expected {v}"
        for p in ("General_StageSpeedUmPerSec", "General_StageSettleMs", "General_StageLimitUm"):
            assert core.hasProperty(xy, p), f"missing XY stage property {p}"
        assert "CellField" in core.getAllowedPropertyValues(cam, "SimType_Pattern")
        defaults = {p: float(core.getProperty(cam, "SimType_CellField" + p)) for p in
                    ("LabelingPctBleaching", "LabelingPctNonBleaching", "MilliActivationRatePerDyePerSec")}
        assert defaults == {"LabelingPctBleaching": 0.0, "LabelingPctNonBleaching": 70.0,
                            "MilliActivationRatePerDyePerSec": 1.43}, f"CellField labelling defaults {defaults}"
        print("CellField/XY stage properties present (defaults: 70% non-bleaching, 1.43e-3/s)")

    # The checks below were tuned on sparse bleaching labelling: 10% bleaching dyes.
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "10")
    core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "0")

    if want("xy"):
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
    core.setProperty(cam, "SimType_CellFieldMilliActivationRatePerDyePerSec", "500")  # dense ON population: shows the MT network
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
    if want("live"):
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
    core.setProperty(cam, "SimType_CellFieldMilliActivationRatePerDyePerSec", "10")
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

    if want("precomputed"):
        t0 = time.time()
        first = stack_frames_at(x0, y0)
        gen_s = time.time() - t0
        stack_frames_at(x0 + 1000.0, y0)
        back = stack_frames_at(x0, y0)
        assert all(np.array_equal(f, g) for f, g in zip(first, back)), \
            "precomputed CellField stack differs after the stage went 1 mm away and back"
        assert any(f.std() > 3 for f in first), "precomputed CellField frames look empty"
        print(f"Precomputed CellField OK: 300-frame stack in {gen_s:.1f} s, byte-identical after a 1 mm excursion")

    # SimType_CellFieldZRangeUm is its own setting: a thin slab renders fewer
    # dyes than the default 7 um one, 0 (no z limit) at least as many.
    def mean_signal(zr):
        core.setProperty(cam, "SimType_CellFieldZRangeUm", str(zr))
        return float(np.mean([f.astype(np.float64).mean() for f in stack_frames_at(x0, y0, n=20)]))
    if want("zrange"):
        thin, default, unlimited = mean_signal(0.2), mean_signal(7), mean_signal(0)
        core.setProperty(cam, "SimType_CellFieldZRangeUm", "7")
        assert thin < default <= unlimited + 1e-9, f"z range 0.2/7/0 um: mean {thin:.3f}/{default:.3f}/{unlimited:.3f} ADU"
        print(f"CellFieldZRangeUm OK: mean frame {thin:.2f} (0.2 um) < {default:.2f} (7 um) <= {unlimited:.2f} ADU (no limit)")

    # ZStage = focal-plane height above the coverslip (+Z focuses up): at
    # +4 um the 7 um slab still holds the cells' dyes, at -4 um (below the
    # coverslip) it holds none.
    def mean_at_z(zpos):
        core.setPosition(z, zpos)
        return float(np.mean([f.astype(np.float64).mean() for f in stack_frames_at(x0, y0, n=20)]))
    if want("zsign"):
        up, down, empty = mean_at_z(4.0), mean_at_z(-4.0), mean_at_z(-20.0)
        core.setPosition(z, 1.5)
        assert up > empty + 1.0 and abs(down - empty) < 0.5, \
            f"Z sign: mean {up:.2f} at +4 um, {down:.2f} at -4 um, {empty:.2f} far below"
        print(f"ZStage sign OK: +4 um sees the cells ({up:.2f} ADU), -4 um below the coverslip does not ({down:.2f} ~ {empty:.2f})")

    # Bleaching dyes run out, non-bleaching (DNA-PAINT-like) sites do not:
    # 300 frames x 20 ms at 1 activation/dye/s (300 snaps: a snap takes its
    # exposure) -- the bleaching-only signal collapses, the non-bleaching one
    # stays flat.
    def early_late(bleach_pct, nonbleach_pct):
        core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", str(bleach_pct))
        core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", str(nonbleach_pct))
        core.setProperty(cam, "SimType_CellFieldMilliActivationRatePerDyePerSec", "1000")
        core.setXYPosition(xy, x0, y0)
        _wait_idle(core, xy)
        core.setProperty(cam, "General_GenerateStack", "1")
        _wait_for_stack(core, cam)
        sig = []
        for _ in range(300):
            core.snapImage()
            sig.append(core.getImage().astype(np.float64).mean() - 100.0)
        return float(np.mean(sig[:30])), float(np.mean(sig[-30:]))
    if want("bleach"):
        b_early, b_late = early_late(10, 0)
        p_early, p_late = early_late(0, 1)
        core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "10")
        core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "0")
        core.setProperty(cam, "SimType_CellFieldMilliActivationRatePerDyePerSec", "10")
        assert b_late < 0.2 * b_early, f"bleaching dyes should run out: {b_early:.2f} -> {b_late:.2f} ADU"
        assert 0.8 < p_late / p_early < 1.25, f"non-bleaching sites should not: {p_early:.2f} -> {p_late:.2f} ADU"
        print(f"Bleaching vs non-bleaching OK: signal {b_early:.2f} -> {b_late:.2f} ADU (bleaching), "
              f"{p_early:.2f} -> {p_late:.2f} ADU (non-bleaching) over 6 s")

    if want("widefield"):
        _widefield_checks(core, cam, xy, x0, y0)
    if want("wfzseq"):
        _zsequence_checks(core, cam, z, "WideField")
    if want("brightfield"):
        _brightfield_checks(core, cam, z)
    # BrightField: foci through and around the cells (above them the
    # defocused images differ too little to tell apart in noise).
    # A bright lamp (16000 photons/px per 20 ms frame, below 16-bit
    # saturation): at the default 800 the single frames are shot-noise
    # limited (~3.5% against ~1-2% cell contrast) and close foci swap.
    if want("bfzseq"):
        core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "800000")
        core.setProperty(cam, "CamParam_GainStdPctPerPixel", "0")
        _zsequence_checks(core, cam, z, "BrightField", (-4.0, -1.5, 1.0, 3.5))
    if want("drift"):
        _drift_checks(core, cam, xy, x0, y0)
    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "80000")
    core.setProperty(cam, "General_BrightFieldQuality", "3")
    core.setProperty(cam, "CamParam_GainStdPctPerPixel", "0.5")
    core.setProperty(cam, "General_ImagingModality", "SuperRes")

    core.setProperty(cam, "General_AcqMode", "Live")
    core.setXYPosition(xy, 0.0, 0.0)
    _wait_idle(core, xy)


def _wf_half_time_s(core, cam):
    """t1/2 from the WideField properties (WidefieldRender.h): B ln2 / (QY sigma Phi)."""
    g = lambda p: float(core.getProperty(cam, "FluoParam_WideField" + p))
    sigma_um2 = np.log(10.0) * 1000.0 * g("ExtinctionCoeff") / 6.02214076e23 * 1e8
    return g("PhotonBudget") * np.log(2.0) / (g("QuantumYield") * sigma_um2 * g("ExcitationPhotonsPerUm2PerSec"))


def _widefield_checks(core, cam, xy, x0, y0):
    for p, v in (("General_ImagingModality", "SuperRes"), ("General_WideFieldUpscaling", 1.0),
                 ("General_WideFieldZPlaneNm", 25.0), ("FluoParam_WideFieldExcitationPhotonsPerUm2PerSec", 4e8),
                 ("FluoParam_WideFieldQuantumYield", 0.7), ("FluoParam_WideFieldPhotonBudget", 5000.0),
                 ("FluoParam_WideFieldExtinctionCoeff", 270000.0)):
        assert core.hasProperty(cam, p), f"missing camera property {p}"
        got = core.getProperty(cam, p)
        assert (got == v) if isinstance(v, str) else abs(float(got) / v - 1) < 1e-9, f"{p} default {got}, expected {v}"
    assert "WideField" in core.getAllowedPropertyValues(cam, "General_ImagingModality")
    t_half = _wf_half_time_s(core, cam)
    assert abs(t_half - 120.0) < 0.2, f"default WideField t1/2 {t_half:.3f} s, expected 120 s"
    reported = float(core.getProperty(cam, "FluoParam_WideFieldHalfTimeSec"))
    assert core.isPropertyReadOnly(cam, "FluoParam_WideFieldHalfTimeSec") and abs(reported / t_half - 1) < 1e-4, \
        f"FluoParam_WideFieldHalfTimeSec {reported} vs {t_half}"
    core.setProperty(cam, "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec", "8e8")
    halved = float(core.getProperty(cam, "FluoParam_WideFieldHalfTimeSec"))
    core.setProperty(cam, "FluoParam_WideFieldPhotonBudget", "0")
    never = float(core.getProperty(cam, "FluoParam_WideFieldHalfTimeSec"))
    core.setProperty(cam, "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec", "4e8")
    core.setProperty(cam, "FluoParam_WideFieldPhotonBudget", "5000")
    assert abs(halved / t_half - 0.5) < 1e-4 and never == -1, f"half time follows: {halved}, budget 0 -> {never}"
    print(f"WideField properties present (defaults give t1/2 = {t_half:.2f} s; FluoParam_WideFieldHalfTimeSec "
          f"reports {reported:.2f} s, {halved:.2f} s at 2x flux, -1 = never at budget 0)")

    offset = float(core.getProperty(cam, "CamParam_OffsetADU"))
    core.setProperty(cam, "General_ImagingModality", "WideField")
    core.setProperty(cam, "General_AcqMode", "Precomputed")
    core.setExposure(50.0)
    core.setXYPosition(xy, x0, y0)
    _wait_idle(core, xy)

    # The bleaching law at 40x the default flux (t1/2 3 s): 3 s of stack (60
    # snaps; a snap takes its exposure) shows a clear decay, the signal well
    # above the noise.
    core.setProperty(cam, "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec", "1.6e10")
    t_half = _wf_half_time_s(core, cam)

    def stack_signal(bleach_pct, nonbleach_pct):
        core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", str(bleach_pct))
        core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", str(nonbleach_pct))
        core.setProperty(cam, "General_GenerateStack", "1")
        _wait_for_stack(core, cam)
        sig = []
        for _ in range(100):
            core.snapImage()
            sig.append(core.getImage().astype(np.float64).mean() - offset)
        return np.array(sig)

    b = stack_signal(20, 0)
    assert "WideField" in core.getProperty(cam, "General_GpuStatus"), core.getProperty(cam, "General_GpuStatus")
    ratio = b[50:70].mean() / b[0:20].mean()
    expect = 2.0 ** (-(50 * 0.05) / t_half)
    assert b[0:20].mean() > 5 and abs(ratio / expect - 1) < 0.03, \
        f"WideField bleaching: frames 50-69 / 0-19 = {ratio:.3f}, expected {expect:.3f} (signal {b[0:20].mean():.2f} ADU)"
    p = stack_signal(0, 70)
    pr = p[-20:].mean() / p[:20].mean()
    assert p[:20].mean() > 5 and abs(pr - 1) < 0.02, f"WideField non-bleaching should stay flat: ratio {pr:.3f}"
    print(f"WideField stack OK: 20% bleaching labelling decays to {ratio:.3f} at {50 * 0.05:.1f} s "
          f"(expected {expect:.3f}, t1/2 {t_half:.1f} s); 70% non-bleaching flat ({pr:.3f})")

    # Live: a world-anchored bleach map. Bright excitation (t1/2 ~ 0.3 s),
    # bleach the FOV, move 30 um away (fresh, bright, then bleaches too) and
    # back (still dim).
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "20")
    core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "0")
    core.setProperty(cam, "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec", "1.6e11")
    core.setProperty(cam, "General_AcqMode", "Live")
    core.setExposure(20.0)

    def live_mean():
        core.snapImage()
        return core.getImage().astype(np.float64).mean() - offset

    def bleach_here(n=60):
        first = np.mean([live_mean() for _ in range(3)])
        for _ in range(n):
            live_mean()
        return first, np.mean([live_mean() for _ in range(3)])

    here0, here1 = bleach_here()
    core.setXYPosition(xy, x0 + 30.0, y0)
    _wait_idle(core, xy)
    live_mean()
    away0, away1 = bleach_here()
    core.setXYPosition(xy, x0, y0)
    _wait_idle(core, xy)
    live_mean()
    back = np.mean([live_mean() for _ in range(3)])
    core.setProperty(cam, "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec", "4e8")
    core.setProperty(cam, "General_ImagingModality", "SuperRes")
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "10")
    assert here0 > 5 and here1 < 0.2 * here0, f"live WideField should bleach: {here0:.2f} -> {here1:.2f} ADU"
    assert away0 > 3 * away1, f"30 um away should be fresh (bright, then bleaching): {away0:.2f} -> {away1:.2f} ADU"
    assert back < 0.25 * here0, f"back at the bleached region it should still be dim: {back:.2f} vs {here0:.2f} ADU"
    print(f"WideField live bleach map OK: {here0:.1f} -> {here1:.1f} ADU here, fresh {away0:.1f} -> {away1:.1f} ADU "
          f"30 um away, still {back:.1f} ADU back here")


def _brightfield_checks(core, cam, z):
    """BrightField (transmitted light): its properties, the lamp's flux scaling
    the frame, defocus changing the cells' contrast, live = precomputed."""
    for p, v in (("General_BrightFieldQuality", 3.0), ("General_BrightFieldCondenserNa", 0.4),
                 ("General_BrightFieldWavelengthNm", 550.0), ("General_BrightFieldPhotonsPerPxPerSec", 80000.0),
                 ("General_BrightFieldSliceUm", -1.0), ("SimType_CellFieldIndexMedium", 1.337),
                 ("SimType_CellFieldIndexCytoplasm", 1.35), ("SimType_CellFieldIndexNucleus", 1.35),
                 ("SimType_CellFieldIndexMicrotubule", 1.48), ("SimType_CellFieldAbsorptionPerUm", 0.0)):
        got = float(core.getProperty(cam, p))
        assert abs(got - v) < 1e-9, f"{p} default {got}, expected {v}"
    assert "BrightField" in core.getAllowedPropertyValues(cam, "General_ImagingModality")
    core.setProperty(cam, "General_ImagingModality", "BrightField")
    core.setProperty(cam, "General_BrightFieldQuality", "1")  # fast: thin object, 6 sources
    core.setProperty(cam, "CamParam_GainStdPctPerPixel", "0")
    core.setExposure(20.0)
    core.setProperty(cam, "General_AcqMode", "Live")

    def avg(n=4):
        acc = None
        for _ in range(n):
            core.snapImage()
            img = core.getImage().astype(np.float64)
            acc = img if acc is None else acc + img
        return acc / n

    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "0")
    dark = avg().mean()
    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "40000")
    a = avg()
    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "80000")
    b = avg()
    ratio = (b.mean() - dark) / (a.mean() - dark)
    assert 1.95 < ratio < 2.05, f"doubling the lamp should double the signal: {ratio:.3f}"
    # A bright lamp (16000 photons/px per 20 ms frame, below 16-bit
    # saturation) and 16-frame averages, so the comparisons below are not
    # limited by shot noise (the default indices give ~1% contrast).
    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "800000")
    core.setPosition(z, 0.0)
    f0 = avg(16)
    core.setPosition(z, -3.0)
    fm = avg(16)
    rel = lambda im: im.std() / (im.mean() - dark)
    corr = lambda u, v: float(((u - u.mean()) * (v - v.mean())).sum() /
                              (np.sqrt(((u - u.mean()) ** 2).sum() * ((v - v.mean()) ** 2).sum()) + 1e-12))
    assert corr(f0, fm) < 0.9, f"defocus should change the image (corr {corr(f0, fm):.3f})"
    core.setProperty(cam, "General_AcqMode", "Precomputed")
    core.setProperty(cam, "General_GenerateStack", "1")
    _wait_for_stack(core, cam)
    pm = avg(16)
    assert corr(pm, fm) > 0.8, f"precomputed and live BrightField should agree (corr {corr(pm, fm):.3f})"
    print(f"BrightField OK: lamp x2 -> signal x{ratio:.3f}, contrast {rel(f0):.3f} (focus 0) / {rel(fm):.3f} "
          f"(-3 um), live vs precomputed corr {corr(pm, fm):.3f}")
    core.setProperty(cam, "General_BrightFieldPhotonsPerPxPerSec", "80000")
    core.setProperty(cam, "General_AcqMode", "Live")
    core.setPosition(z, 1.5)


def _zsequence_checks(core, cam, z, modality, positions=(0.5, 1.25, 2.0, 2.75)):
    """A hardware z stack: the ZStage is sequenceable, the camera takes one
    sequence position per frame (live and precomputed modes), and the stage
    returns to where it was when the sequence stops."""
    positions = list(positions)
    core.setProperty(cam, "General_ImagingModality", modality)
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "0")
    core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "70")
    core.setExposure(20.0)
    # Positions inside the cells (the dome top is the nucleus top + 0.5 um, at most ~5 um since 2026-10-05: 3.5 and
    # 5 um held few dyes and their frames matched at random). A thin slab makes every position a distinct dye layer (the Gaussian
    # WideField PSF, what the Linux test build has, ignores defocus).
    z_range = core.getProperty(cam, "SimType_CellFieldZRangeUm")
    core.setProperty(cam, "SimType_CellFieldZRangeUm", "0.5")
    assert core.isStageSequenceable(z) and core.getStageSequenceMaxLength(z) >= len(positions)
    z_before = core.getPosition(z)

    def norm(a):
        a = a - a.mean()
        return a / (np.sqrt((a * a).sum()) + 1e-12)

    for mode in ("Live", "Precomputed"):
        core.setProperty(cam, "General_AcqMode", mode)
        refs = []
        for p in positions:
            core.setPosition(z, p)
            if mode == "Precomputed":
                core.setProperty(cam, "General_GenerateStack", "1")
                _wait_for_stack(core, cam)
            acc = None
            for _ in range(4):
                core.snapImage()
                img = core.getImage().astype(np.float64)
                acc = img if acc is None else acc + img
            refs.append(norm(acc))
        core.setPosition(z, z_before)
        core.loadStageSequence(z, positions)
        core.startStageSequence(z)
        n = 2 * len(positions)
        core.startSequenceAcquisition(n, 0, True)
        frames = []
        t0 = time.time()
        while len(frames) < n:
            if core.getRemainingImageCount() > 0:
                frames.append(core.popNextImage().astype(np.float64))
            elif time.time() - t0 > 300:
                sys.exit("z sequence acquisition timed out")
            else:
                time.sleep(0.005)
        core.stopSequenceAcquisition()
        core.stopStageSequence(z)
        picks = [int(np.argmax([(norm(f) * r).sum() for r in refs])) for f in frames]
        expect = [k % len(positions) for k in range(n)]
        assert picks == expect, f"{modality} {mode} z sequence: frames matched positions {picks}, expected {expect}"
        assert abs(core.getPosition(z) - z_before) < 1e-9, f"stage should return to {z_before}, at {core.getPosition(z)}"
        print(f"z sequence OK ({modality}, {mode}): {n} frames at positions {[positions[i] for i in picks]}, "
              f"stage back at {z_before} um")
    core.setProperty(cam, "General_AcqMode", "Live")
    core.setProperty(cam, "SimType_CellFieldZRangeUm", z_range)
    core.setProperty(cam, "General_ImagingModality", "SuperRes")
    core.setProperty(cam, "SimType_CellFieldLabelingPctBleaching", "10")
    core.setProperty(cam, "SimType_CellFieldLabelingPctNonBleaching", "0")


if __name__ == "__main__":
    import argparse
    import builtins

    from pymmcore_plus import CMMCorePlus

    ap = argparse.ArgumentParser(description="CellField / XY stage checks; every section by default.")
    ap.add_argument("--only", help="comma-separated sections: " + ",".join(SECTIONS))
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None
    if only and not only <= set(SECTIONS):
        sys.exit(f"unknown section(s) {sorted(only - set(SECTIONS))}; known: {', '.join(SECTIONS)}")
    # Every line printed carries the time since the previous one: what each check costs.
    _t = [time.time(), time.time()]
    _print = builtins.print

    def _timed(*a, **k):
        now = time.time()
        _print(f"[{now - _t[0]:6.1f} s]", *a, **k)
        _t[0] = now
    builtins.print = _timed

    core = CMMCorePlus()
    dirs = [d for d in (os.environ.get("ADAPTER_DIR"), os.environ.get("MM_DIR")) if d]
    if not dirs:
        sys.exit("Set ADAPTER_DIR to the directory holding the inSiliScope adapter.")
    core.setDeviceAdapterSearchPaths(dirs)
    run_checks(core, only=only)
    print(f"All CellField / XY stage checks passed ({time.time() - _t[1]:.0f} s in total).")
