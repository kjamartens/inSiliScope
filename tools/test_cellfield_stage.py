"""CellField pattern + XYStage checks (spec/PORT.md 10.5), with pymmcore-plus.

Exercises: the XY stage device (Busy while moving, move time ~ distance/speed,
position readback, MM's TransposeMirrorX flipping the direction), the camera's
CellField pattern rendering dyes of the insiliscope world through the unchanged
render pipeline, a known feature shifting by the expected pixels between two
stage positions (live mode), the illumination history (a repeat stack at a
spot continues it: dSTORM dyes used up, DNA-PAINT sites not; live: bleach
here, a fresh region 30 um away, still dim back here), the microtubules' label modes
(DNA-PAINT sites do not run out; a WideField-mode mEGFP mean field bleaches with
the half time its dye fields give), a hardware z stack (the ZStage's sequence, one position per camera frame),
and the BrightField modality (lamp flux, defocus contrast, live = precomputed,
z sequence).

The adapter is the inSiliScope hub with its devices (tools/isc_mm.py loads them; spec/MM_DEVICES.md); the
precomputed stacks are the camera's Test rows (ISC_TEST=1). BrightField = the TransmittedLamp's shutter open and the
Lasers' closed.

Standalone (the Linux test build works too: tools/build_adapter_linux.sh):
    ADAPTER_DIR=<dir with the adapter> python tools/test_cellfield_stage.py [--drift]
Run it after tools/test_insiliscope.py, then once more with --drift (the sample drift checks alone): three runs, each
under 5 minutes; run_checks(core) for a core of your own.
Uses the Renderer's PsfModel=Gaussian, so no JVM is needed.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import brightfield, custom_microtubule_dye, load_scope, search_paths  # noqa: E402

import numpy as np  # noqa: E402


def _wait_idle(core, label, timeout_s=30.0):
    t0 = time.time()
    while core.deviceBusy(label):
        if time.time() - t0 > timeout_s:
            sys.exit(f"{label} still busy after {timeout_s} s")
        time.sleep(0.002)
    return time.time() - t0


def _wait_for_stack(core, cam, timeout_s=600.0):
    t0 = time.time()
    while not core.getProperty("Camera", "Test_StackGenerationStatus").startswith("Ready"):
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
    """Simulation/Drift.cpp's random walk in Python: the sample drift (x, y, z nm) of each frame."""
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
    """Sample drift (the SampleHolder's Drift*): the properties are there (the linear DriftNmPerSec is gone) and wired
    into both modalities. The physics (exact sub-pixel shift, focus grid, blinks, C++ = JS) is ctest drift / widefield /
    brightfield and scope_parity. Fluorescence uses the mEGFP WideField label (one continuous image; a huge photon
    budget, so the illumination history of the repeated stacks does not dim it).
    Only the adapter's own paths (the shifts themselves, BrightField stacks and the directed part are checked exactly
    in ctest drift / brightfield and scope_parity):
    - Precomputed (Fluorescence): frame 9 of a drifting 10-frame stack is the still stack's frame 9 moved by the
      seed's drift path, the path the cli and viewer take (_drift_nm).
    - Live, each modality: frame 9 of a sequence acquisition moves by the same path as the stacks' frame 9."""
    assert not core.hasProperty("SampleHolder", "DriftNmPerSec"), "DriftNmPerSec should be gone"
    for p, v in (("DriftXyNmPerSqrtSec", 0.0), ("DriftZNmPerSqrtSec", 0.0),
                 ("DriftXySpeedNmPerSec", 0.0), ("DriftZSpeedNmPerSec", 0.0),
                 ("DriftXyAngleDeg", -1.0), ("DriftXyAngleWanderDeg", 180.0), ("DriftZAngleWanderDeg", 90.0),
                 ("DriftSpeedWanderPct", 0.0), ("DriftWanderTimeSec", 60.0)):
        assert core.hasProperty("SampleHolder", p) and float(core.getProperty("SampleHolder", p)) == v, \
            f"SampleHolder.{p} missing or not {v}"
    assert core.getProperty("SampleHolder", "DriftZDirection") == "Random"
    # The preset (Basic): sets the speeds and walks; a member set by hand makes it Custom.
    assert core.getProperty("SampleHolder", "DriftPreset") == "Off"
    for name, speed, walk in (("Low", 2, 0.4), ("Medium", 5, 1), ("High", 25, 5), ("Extreme", 250, 50), ("Off", 0, 0)):
        core.setProperty("SampleHolder", "DriftPreset", name)
        got = [float(core.getProperty("SampleHolder", p)) for p in
               ("DriftXySpeedNmPerSec", "DriftZSpeedNmPerSec", "DriftXyNmPerSqrtSec", "DriftZNmPerSqrtSec")]
        assert got == [speed, speed, walk, walk], f"DriftPreset {name}: {got}"
    core.setProperty("SampleHolder", "DriftZNmPerSqrtSec", "3")
    assert core.getProperty("SampleHolder", "DriftPreset") == "Custom", "a drift set by hand should make the preset Custom"
    core.setProperty("SampleHolder", "DriftPreset", "Off")
    print("Drift preset OK: Off/Low/Medium/High/Extreme set the speeds and walks; a hand-set value makes it Custom")
    core.setXYPosition(xy, x0, y0)  # the field with structure
    _wait_idle(core, xy)
    _label(core, cam, **GFP)
    custom_microtubule_dye(core, PhotonBudget="1e9")
    seed = int(core.getProperty("Hub", "RandomSeed"))
    px = float(core.getProperty("Camera", "PixelSizeNm"))
    # 200 nm/sqrt(s) (the property's maximum) and 400 ms frames: frame 9 (3.6 s) sits ~0.38 um (~4 px rms per axis)
    # from frame 0. A snap of a precomputed stack takes its exposure: 10 snaps = 4 s per stack.
    sxy, sz, exp_ms, k = 200.0, 30.0, 400.0, 9
    path = _drift_nm(seed, k + 1, exp_ms / 1000.0, sxy, sz)
    expect = (round(path[k][1] / px), round(path[k][0] / px))

    def drift(on):
        core.setProperty("SampleHolder", "DriftXyNmPerSqrtSec", str(sxy if on else 0))
        core.setProperty("SampleHolder", "DriftZNmPerSqrtSec", str(sz if on else 0))

    def stack_frame():
        core.setProperty("Camera", "Test_GenerateStack", "1")
        _wait_for_stack(core, cam)
        for _ in range(k + 1):
            core.snapImage()
        return core.getImage().astype(np.float64)

    def setup(modality):
        brightfield(core, modality == "BrightField")
        if modality == "BrightField":
            # Single frames: unstained cells (~0.5% contrast) sit below the shot noise, so the cells absorb here;
            # 16000 photons/px, no saturation.
            core.setProperty("Renderer", "BrightFieldQuality", "1")
            core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", str(16000.0 / (exp_ms / 1000.0)))
            core.setProperty("CellField", "AbsorptionPerUm", "0.3")

    core.setProperty("Camera", "Test_StackLength", str(k + 1))
    core.setProperty("Camera", "Test_AcqMode", "Precomputed")
    core.setExposure(exp_ms)
    for modality in ("Fluorescence",):
        setup(modality)
        drift(False)
        still = stack_frame()
        drift(True)
        moved = stack_frame()
        dy, dx, e = _ls_shift(still, moved)
        assert abs(dy - expect[0]) <= 1 and abs(dx - expect[1]) <= 1, \
            f"{modality}: stack frame {k} moved by ({dy}, {dx}) px, the drift path says {expect}"
        print(f"Drift OK ({modality}, precomputed): frame {k} moved by ({dy}, {dx}) px (path {expect})")

    # Live: a 10-frame sequence acquisition at 200 ms restarts the drift at its first frame, so its frame 9 must move
    # by the same seed path as the stacks' frame 9 (live = precomputed).
    core.setProperty("Camera", "Test_AcqMode", "Live")
    for modality in ("Fluorescence", "BrightField"):
        setup(modality)
        drift(True)
        time.sleep(0.5)  # the live scene is built
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
        assert abs(dy - expect[0]) <= 1 and abs(dx - expect[1]) <= 1, \
            f"{modality} live: frame {k} of a sequence moved by ({dy}, {dx}) px, the drift path says {expect}"
        print(f"Drift OK ({modality}, live): sequence frame {k} moved by ({dy}, {dx}) px (path {expect})")

    drift(False)
    core.setProperty("CellField", "AbsorptionPerUm", "0")
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "80000")
    core.setProperty("Renderer", "BrightFieldQuality", "3")
    brightfield(core, False)
    _label(core, cam, **GFP)  # the library dye again (the photon budget)


def _label(core, cam, mode, dye="Typical", pct=None, imager=None):
    """The microtubules' label: the experiment's mode (Fluorophores), their dye (loads its light preset), labelling,
    imager (DNA-PAINT)."""
    core.setProperty("Fluorophores", "Mode", mode)
    core.setProperty("CellField", "Microtubules_Label", dye)
    if pct is not None:
        core.setProperty("CellField", "Microtubules_LabelingPct", str(pct))
    if imager is not None:
        core.setProperty("CellField", "Microtubules_ImagerNm", str(imager))


# The checks' default label: DNA-PAINT ATTO 655 sites (persistent; the
# imager's concentration sets both the binding rate and a flat background).
PAINT = dict(mode="DNA-PAINT", dye="ATTO655", pct=70, imager=1.0)
# A continuous image of the microtubule network: mEGFP in WideField mode.
GFP = dict(mode="WideField", dye="mEGFP")


def run_checks(core, cam="Camera", xy="XYStage", z="ZStage", drift_only=False):
    """Every check but the drift; drift_only: the setup, the search for a FOV with structure and the drift checks
    (a run of their own: together they would pass 5 minutes). Loads the hub and its devices (labels = device names)."""
    load_scope(core, seed=7, fov="128x128")
    assert abs(core.getPosition(z) - 0.5) < 1e-9, f"ZStage should start at 0.5 um, got {core.getPosition(z)}"
    print("ZStage starts at 0.5 um")
    core.setPosition(z, 1.5)  # focal plane 1.5 um above the coverslip (was the old default view)

    # ---- property surface -------------------------------------------------
    for p in ("ChunkSizeUm", "Occupancy", "Packing", "CellDiameterMinUm", "CellDiameterMaxUm",
              "MicrotubuleDensityPerUm2", "FocusHeightUm", "ZRangeUm", "Microtubules_Label", "Microtubules_Mode",
              "Microtubules_LabelingPct", "Microtubules_ImagerNm"):
        assert core.hasProperty("CellField", p), f"missing CellField property {p}"
    # Nucleus shape and microtubule start/end (2026-10-05): the core's defaults.
    for p, v in (("NucBaseMinUm", 0.4), ("NucBaseMaxUm", 0.9), ("NucIrregMin", 0.03), ("NucIrregMax", 0.2),
                 ("NucBendMin", 0.0), ("NucBendMax", 0.3), ("NucSmooth", 2.5), ("NucThickIrreg", 0.1),
                 ("NucAsym", 0.5), ("NucWidestMin", 0.2), ("NucWidestMax", 0.4), ("MicrotubuleStartDecayPct", 1.6),
                 ("MicrotubuleEndDecayPct", 20.0), ("MicrotubuleDirKappa", 1.5)):
        assert core.hasProperty("CellField", p), f"missing CellField property {p}"
        got = float(core.getProperty("CellField", p))
        assert abs(got - v) < 1e-9, f"{p} default {got}, expected {v}"
    for p in ("StageSpeedUmPerSec", "StageSettleMs", "StageLimitUm"):
        assert core.hasProperty(xy, p), f"missing XY stage property {p}"
    defaults = {p: core.getProperty("CellField", "Microtubules_" + p) for p in
                ("Label", "Mode", "LabelingPct", "ImagerNm")}
    defaults["Dye"] = core.getProperty("Fluorophores", "Microtubules_EffectiveDye")
    assert defaults["Dye"] == "ATTO655 (DNA-PAINT)" and defaults["Label"] == "Typical" and defaults["Mode"] == "Global" and \
        float(defaults["LabelingPct"]) == 70 and float(defaults["ImagerNm"]) == 1.0, f"label defaults {defaults}"
    assert core.getProperty("Lasers", "Preset") == "PAINT-640", core.getProperty("Lasers", "Preset")
    print("CellField/XY stage properties present (defaults: DNA-PAINT ATTO 655, 70% of sites, 1 nM imager)")
    _label(core, cam, **PAINT)

    # ---- XY stage motion --------------------------------------------------
    core.setXYPosition(xy, 0.0, 0.0)
    _wait_idle(core, xy)
    core.setProperty("XYStage", "StageSpeedUmPerSec", "1000")
    core.setProperty("XYStage", "StageSettleMs", "20")
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
    core.setProperty("XYStage", "StageSpeedUmPerSec", "100000")
    print(f"XY stage OK: 500 um move took {took:.3f} s at 1000 um/s, Busy during, arrived at {pos}")

    # ---- live mode: a feature moves by the stage step ----------------------
    core.setProperty("Renderer", "PsfModel", "Gaussian")
    core.setProperty("SampleHolder", "BackgroundPhotonsPerSec", "0")
    # No static per-pixel pattern: it would correlate at zero shift.
    for p in ("sCMOS_GainStdPctPerPixel", "sCMOS_ReadNoiseStdPctPerPixel", "OffsetStdADU"):
        core.setProperty("Camera", p, "0")
    _label(core, cam, **GFP)  # every dye at once: shows the MT network
    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.setExposure(10.0)
    px_um = float(core.getProperty("Camera", "PixelSizeNm")) / 1000.0

    # Find a FOV with structure: the brightest of a few spots.
    best = None
    spots = [(0.0, 0.0), (26.0, 13.0)] if drift_only else [(x, y) for x in (0.0, 13.0, 26.0, 39.0) for y in (0.0, 13.0, 26.0)]
    for x, y in spots:
        if True:
            core.setXYPosition(xy, x, y)
            _wait_idle(core, xy)
            core.snapImage()  # a frame at the new pose
            s = _live_sum(core, 5).std()
            if best is None or s > best[0]:
                best = (s, x, y)
    s, x0, y0 = best
    assert s > 5.0, f"no structure found in any test FOV (best std {s:.2f}): CellField renders nothing?"
    if drift_only:
        _drift_checks(core, cam, xy, x0, y0)
        return
    core.setXYPosition(xy, x0, y0)
    _wait_idle(core, xy)
    core.snapImage()
    a = _live_sum(core, 40)
    step_um = round(2.0 / px_um) * px_um   # about 2 um, a whole number of pixels (97.45 nm: 20 px)
    core.setXYPosition(xy, x0 + step_um, y0)
    _wait_idle(core, xy)
    core.snapImage()
    b = _live_sum(core, 40)
    dy, dx, peak = _shift(a, b)
    expect = -round(step_um / px_um)
    assert (dy, dx) == (0, expect), f"stage +{step_um} um in x: image shift (dy, dx) = ({dy}, {dx}), expected (0, {expect})"
    print(f"Live CellField OK: stage +{step_um:.3f} um in x moved the structure by {dx} px (expected {expect}), "
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

    # ---- precomputed: stacks continue the illumination history --------------
    _label(core, cam, **PAINT)
    core.setProperty("Camera", "Test_AcqMode", "Precomputed")
    core.setExposure(20.0)

    def stack_frames_at(x, y, n=5):
        core.setXYPosition(xy, x, y)
        _wait_idle(core, xy)
        core.setProperty("Camera", "Test_GenerateStack", "1")
        _wait_for_stack(core, cam)
        frames = []
        for _ in range(n):
            core.snapImage()
            frames.append(core.getImage().copy())
        return frames

    # A stack lights its FOV for 1000 x 20 ms: the next stack there starts 20 s
    # later on its dyes' clocks. DNA-PAINT sites never run out (the same level,
    # other blinks); dSTORM dyes at a fresh spot start in their initial ON
    # phase and are largely used up by a second stack there.
    def mean_of(frames):
        return float(np.mean([f.astype(np.float64).mean() for f in frames])) - 100.0
    t0 = time.time()
    first = stack_frames_at(x0, y0)
    gen_s = time.time() - t0
    stack_frames_at(x0 + 1000.0, y0)
    back = stack_frames_at(x0, y0)
    assert any(f.std() > 3 for f in first), "precomputed CellField frames look empty"
    assert not all(np.array_equal(f, g) for f, g in zip(first, back)), "a repeat stack should continue the history"
    assert abs(mean_of(back) / mean_of(first) - 1) < 0.15, \
        f"DNA-PAINT repeat stack: {mean_of(first):.2f} -> {mean_of(back):.2f} ADU (sites never run out)"
    _label(core, cam, mode="dSTORM", dye="AF647")
    d1, d2 = mean_of(stack_frames_at(x0 + 500.0, y0, n=20)), mean_of(stack_frames_at(x0 + 500.0, y0, n=20))
    _label(core, cam, **PAINT)
    assert d2 < 0.5 * d1, f"dSTORM at a fresh spot, then again: {d1:.1f} -> {d2:.1f} ADU (initial ON, then used up)"
    print(f"Precomputed CellField OK: 1000-frame stack in {gen_s:.1f} s; a repeat stack continues the history "
          f"(DNA-PAINT {mean_of(first):.1f} -> {mean_of(back):.1f} ADU, dSTORM {d1:.1f} -> {d2:.1f} ADU)")

    # The CellField's ZRangeUm is its own setting: a thin slab renders fewer
    # dyes than the default 7 um one, 0 (no z limit) about as many (each
    # stack is another stretch of the DNA-PAINT blinks: 3 % tolerance).
    def mean_signal(zr):
        core.setProperty("CellField", "ZRangeUm", str(zr))
        return float(np.mean([f.astype(np.float64).mean() for f in stack_frames_at(x0, y0, n=20)]))
    thin, default, unlimited = mean_signal(0.2), mean_signal(7), mean_signal(0)
    core.setProperty("CellField", "ZRangeUm", "7")
    assert thin < default <= 1.03 * unlimited, f"z range 0.2/7/0 um: mean {thin:.3f}/{default:.3f}/{unlimited:.3f} ADU"
    print(f"CellFieldZRangeUm OK: mean frame {thin:.2f} (0.2 um) < {default:.2f} (7 um) ~ {unlimited:.2f} ADU (no limit)")

    # ZStage = focal-plane height above the coverslip (+Z focuses up): at
    # +4 um the 7 um slab still holds the cells' dyes, at -4 um (below the
    # coverslip) it holds none.
    def mean_at_z(zpos):
        core.setPosition(z, zpos)
        return float(np.mean([f.astype(np.float64).mean() for f in stack_frames_at(x0, y0, n=20)]))
    up, down, empty = mean_at_z(4.0), mean_at_z(-4.0), mean_at_z(-20.0)
    core.setPosition(z, 1.5)
    assert up > empty + 1.0 and abs(down - empty) < 0.5, \
        f"Z sign: mean {up:.2f} at +4 um, {down:.2f} at -4 um, {empty:.2f} far below"
    # DNA-PAINT sites do not run out: over the first 300 frames of the stack
    # the signal stays flat (the bleaching side is the WideField check below;
    # a snap waits out its exposure, so few frames are read).
    def early_late():
        core.setXYPosition(xy, x0, y0)
        _wait_idle(core, xy)
        core.setProperty("Camera", "Test_GenerateStack", "1")
        _wait_for_stack(core, cam)
        sig = []
        for _ in range(300):
            core.snapImage()
            sig.append(core.getImage().astype(np.float64).mean() - 100.0)
        return float(np.mean(sig[:100])), float(np.mean(sig[-100:]))
    p_early, p_late = early_late()
    assert p_early > 0.5 and 0.8 < p_late / p_early < 1.25, f"DNA-PAINT sites should not run out: {p_early:.2f} -> {p_late:.2f} ADU"
    print(f"DNA-PAINT OK: signal {p_early:.2f} -> {p_late:.2f} ADU over {300 * core.getExposure() / 1000:.0f} s")

    print(f"ZStage sign OK: +4 um sees the cells ({up:.2f} ADU), -4 um below the coverslip does not ({down:.2f} ~ {empty:.2f})")

    _widefield_checks(core, cam, xy, x0, y0)
    # The z sequences and BrightField at the FOV with the most structure (the
    # history check left the stage at a spot of its own).
    core.setXYPosition(xy, x0, y0)
    _wait_idle(core, xy)
    _zsequence_checks(core, cam, z, "Fluorescence")
    _brightfield_checks(core, cam, z)
    # BrightField: foci through and around the cells (above them the
    # defocused images differ too little to tell apart in noise).
    # A bright lamp (16000 photons/px per 20 ms frame, below 16-bit
    # saturation): at the default 800 the single frames are shot-noise
    # limited (~3.5% against ~1-2% cell contrast) and close foci swap.
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "800000")
    _zsequence_checks(core, cam, z, "BrightField", (-4.0, -1.5, 1.0, 3.5))
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "80000")
    core.setProperty("Renderer", "BrightFieldQuality", "3")
    core.setProperty("Camera", "sCMOS_GainStdPctPerPixel", "0.5")
    brightfield(core, False)
    _label(core, cam, mode="DNA-PAINT", pct=70, imager=1.0)  # the defaults

    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.setXYPosition(xy, 0.0, 0.0)
    _wait_idle(core, xy)


def _wf_half_time_s(core, cam):
    """t1/2 of the microtubules' dye from its fields: budget ln2 / emitted photons per second
    (detected per second / (the objective's collection efficiency x the detected fraction of the emission
    spectrum: dichroic, filter, QE))."""
    g = lambda p: float(core.getProperty("Fluorophores", "Microtubules_" + p))
    r = float(core.getProperty("Objective", "NA")) / float(core.getProperty("Objective", "ImmersionIndex"))
    eta = 0.5 * (1.0 - np.sqrt(1.0 - min(1.0, r) ** 2))
    budget = float(core.getProperty("Fluorophores", "Dye1_PhotonBudget"))   # the microtubules' custom dye
    return budget * np.log(2.0) / (g("PhotonsPerSecOn") / (eta * g("DetectedPct") / 100.0))


def _widefield_checks(core, cam, xy, x0, y0):
    for p, v in (("WideFieldUpscaling", 1.0), ("WideFieldZPlaneNm", 25.0), ("MeanFieldDensityPerUm2", 20.0),
                 ("MeanFieldSlabNm", 500.0), ("MeanFieldMaxEmitters", 5000.0)):
        assert core.hasProperty("Renderer", p), f"missing Renderer property {p}"
        got = core.getProperty("Renderer", p)
        assert abs(float(got) / v - 1) < 1e-9, f"{p} default {got}, expected {v}"

    offset = float(core.getProperty("Camera", "OffsetADU"))
    _label(core, cam, **GFP)
    # A fresh spot with cells (the checks above lit (x0, y0) for over a minute: at t1/2 = 6 s its dyes are gone; one
    # 20 ms live snap per candidate lights it for ~0.3 % of a half time). Away from the illumination-history spots below.
    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.setExposure(20.0)
    wx = wy = None
    for k in range(40):
        core.setXYPosition(xy, x0 - 200.0 - 13.0 * k, y0 - 60.0)
        _wait_idle(core, xy)
        core.snapImage()
        if core.getImage().astype(np.float64).mean() - offset > 20:
            wx, wy = core.getXYPosition(xy)
            break
    assert wx is not None, "no spot with cells found for the WideField bleaching check"
    core.setProperty("Camera", "Test_AcqMode", "Precomputed")
    core.setExposure(50.0)
    # The bleaching law: a photon budget that gives t1/2 = 6 s at the
    # preset's 488 nm flux, so the first 130 frames (6.5 s; a snap waits out
    # its exposure) show a clear decay. Expected: the mean of 2^(-t/t1/2)
    # over each window's frame mid-times.
    T_HALF = 6.0
    custom_microtubule_dye(core)   # the library mEGFP as a custom dye: its photon budget is editable
    t_lib = _wf_half_time_s(core, cam)
    budget = float(core.getProperty("Fluorophores", "Dye1_PhotonBudget")) * T_HALF / t_lib
    custom_microtubule_dye(core, PhotonBudget=f"{budget:.6g}")
    t_half = _wf_half_time_s(core, cam)
    assert abs(t_half / T_HALF - 1) < 1e-3, f"budget {budget:.6g} gives t1/2 {t_half:.3f} s"
    core.setProperty("Camera", "Test_GenerateStack", "1")
    _wait_for_stack(core, cam)
    sig = []
    for _ in range(130):
        core.snapImage()
        sig.append(core.getImage().astype(np.float64).mean() - offset)
    b = np.array(sig)
    status = core.getProperty("Renderer", "GpuStatus")
    w0, w1 = np.arange(0, 10), np.arange(115, 125)
    ratio = b[w1].mean() / b[w0].mean()
    decay = lambda w: np.mean(2.0 ** (-((w + 0.5) * 0.05) / t_half))
    expect = decay(w1) / decay(w0)
    assert b[w0].mean() > 5 and abs(ratio / expect - 1) < 0.03, \
        f"WideField bleaching: frames 115-124 / 0-9 = {ratio:.3f}, expected {expect:.3f} (signal {b[w0].mean():.2f} ADU)"
    print(f"WideField-mode mEGFP OK ({status}): decays to {ratio:.3f} at 6 s (expected {expect:.3f}, t1/2 "
          f"{t_half:.1f} s from its fields; library t1/2 {t_lib:.1f} s)")

    # Live: the illumination history. A small budget (t1/2 ~0.4 s at the
    # preset): bleach the FOV, move 30 um away (fresh, bright, then bleaches
    # too) and back (still dim). Snaps and sequences light the sample; an idle
    # live loop does not.
    custom_microtubule_dye(core, PhotonBudget="2000")
    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.setExposure(20.0)

    def live_mean():
        core.snapImage()
        return core.getImage().astype(np.float64).mean() - offset

    def bleach_here(n=60):
        first = np.mean([live_mean() for _ in range(3)])
        for _ in range(n):
            live_mean()
        return first, np.mean([live_mean() for _ in range(3)])

    # Two spots 30 um apart, both with cells, away from what the checks above
    # lit (one 20 ms snap each to look: ~3 % of a half time).
    def peek(x, y):
        core.setXYPosition(xy, x, y)
        _wait_idle(core, xy)
        return live_mean()
    lx = ly = None
    for k in range(40):
        cx, cy = x0 + 200.0 + 13.0 * k, y0 + 60.0
        if peek(cx, cy) > 20 and peek(cx + 30.0, cy) > 20:
            lx, ly = cx, cy
            break
    assert lx is not None, "no pair of spots with cells found for the illumination-history check"
    core.setXYPosition(xy, lx, ly)
    _wait_idle(core, xy)
    here0, here1 = bleach_here()
    core.setXYPosition(xy, lx + 30.0, ly)
    _wait_idle(core, xy)
    away0, away1 = bleach_here()
    core.setXYPosition(xy, lx, ly)
    _wait_idle(core, xy)
    back = np.mean([live_mean() for _ in range(3)])
    time.sleep(2.0)   # idle: nothing acquires, nothing bleaches
    idle = np.mean([live_mean() for _ in range(3)])
    _label(core, cam, **GFP)  # reload the library fields
    _label(core, cam, **PAINT)
    assert here0 > 5 and here1 < 0.2 * here0, f"live WideField should bleach: {here0:.2f} -> {here1:.2f} ADU"
    assert away0 > 3 * away1, f"30 um away should be fresh (bright, then bleaching): {away0:.2f} -> {away1:.2f} ADU"
    assert back < 0.25 * here0, f"back at the bleached region it should still be dim: {back:.2f} vs {here0:.2f} ADU"
    assert idle > 0.8 * back, f"2 s idle should not bleach: {back:.2f} -> {idle:.2f} ADU"
    print(f"Illumination history OK: {here0:.1f} -> {here1:.1f} ADU here, fresh {away0:.1f} -> {away1:.1f} ADU "
          f"30 um away, still {back:.1f} ADU back here, {idle:.1f} ADU after 2 s idle")


def _brightfield_checks(core, cam, z):
    """BrightField (transmitted light): its properties, the lamp's flux scaling
    the frame, defocus changing the cells' contrast, live = precomputed."""
    for d, p, v in (("Renderer", "BrightFieldQuality", 3.0), ("TransmittedLamp", "CondenserNA", 0.4),
                    ("TransmittedLamp", "WavelengthNm", 550.0), ("TransmittedLamp", "IntensityPhotonsPerPxPerSec", 80000.0),
                    ("Renderer", "BrightFieldSliceUm", -1.0), ("CellField", "IndexMedium", 1.337),
                    ("CellField", "IndexCytoplasm", 1.35), ("CellField", "IndexNucleus", 1.35),
                    ("CellField", "IndexMicrotubule", 1.48), ("CellField", "AbsorptionPerUm", 0.0)):
        got = float(core.getProperty(d, p))
        assert abs(got - v) < 1e-9, f"{d}.{p} default {got}, expected {v}"
    brightfield(core)
    core.setProperty("Renderer", "BrightFieldQuality", "1")  # fast: thin object, 6 sources
    core.setProperty("Camera", "sCMOS_GainStdPctPerPixel", "0")
    core.setExposure(20.0)
    core.setProperty("Camera", "Test_AcqMode", "Live")

    def avg(n=4):
        acc = None
        for _ in range(n):
            core.snapImage()
            img = core.getImage().astype(np.float64)
            acc = img if acc is None else acc + img
        return acc / n

    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "0")
    dark = avg().mean()
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "40000")
    a = avg()
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "80000")
    b = avg()
    ratio = (b.mean() - dark) / (a.mean() - dark)
    assert 1.95 < ratio < 2.05, f"doubling the lamp should double the signal: {ratio:.3f}"
    # A bright lamp (16000 photons/px per 20 ms frame, below 16-bit
    # saturation) and 16-frame averages, so the comparisons below are not
    # limited by shot noise (the default indices give ~1% contrast).
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "800000")
    core.setPosition(z, 0.0)
    f0 = avg(16)
    core.setPosition(z, -3.0)
    fm = avg(16)
    rel = lambda im: im.std() / (im.mean() - dark)
    corr = lambda u, v: float(((u - u.mean()) * (v - v.mean())).sum() /
                              (np.sqrt(((u - u.mean()) ** 2).sum() * ((v - v.mean()) ** 2).sum()) + 1e-12))
    assert corr(f0, fm) < 0.9, f"defocus should change the image (corr {corr(f0, fm):.3f})"
    core.setProperty("Camera", "Test_AcqMode", "Precomputed")
    core.setProperty("Camera", "Test_GenerateStack", "1")
    _wait_for_stack(core, cam)
    pm = avg(16)
    assert corr(pm, fm) > 0.8, f"precomputed and live BrightField should agree (corr {corr(pm, fm):.3f})"
    print(f"BrightField OK: lamp x2 -> signal x{ratio:.3f}, contrast {rel(f0):.3f} (focus 0) / {rel(fm):.3f} "
          f"(-3 um), live vs precomputed corr {corr(pm, fm):.3f}")
    core.setProperty("TransmittedLamp", "IntensityPhotonsPerPxPerSec", "80000")
    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.setPosition(z, 1.5)


def _zsequence_checks(core, cam, z, modality, positions=(0.5, 1.25, 2.0, 2.75)):
    """A hardware z stack: the ZStage is sequenceable, the camera takes one
    sequence position per frame (live and precomputed modes), and the stage
    returns to where it was when the sequence stops."""
    positions = list(positions)
    brightfield(core, modality == "BrightField")
    if modality == "Fluorescence":
        _label(core, cam, **GFP)
        # A budget that never runs out: the stacks above have bleached this
        # spot's mEGFP (its clock is minutes), and the references and the
        # sequence must see the same dyes.
        custom_microtubule_dye(core, PhotonBudget="1e12")
    core.setExposure(20.0)
    # Positions inside the cells (the dome top is the nucleus top + 0.5 um, at most ~5 um since 2026-10-05: 3.5 and
    # 5 um held few dyes and their frames matched at random). A thin slab makes every position a distinct dye layer (the Gaussian
    # WideField PSF, what the Linux test build has, ignores defocus).
    z_range = core.getProperty("CellField", "ZRangeUm")
    core.setProperty("CellField", "ZRangeUm", "0.5")
    assert core.isStageSequenceable(z) and core.getStageSequenceMaxLength(z) >= len(positions)
    z_before = core.getPosition(z)

    def norm(a):
        a = a - a.mean()
        return a / (np.sqrt((a * a).sum()) + 1e-12)

    for mode in ("Live", "Precomputed"):
        core.setProperty("Camera", "Test_AcqMode", mode)
        refs = []
        for p in positions:
            core.setPosition(z, p)
            if mode == "Precomputed":
                core.setProperty("Camera", "Test_GenerateStack", "1")
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
    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.setProperty("CellField", "ZRangeUm", z_range)
    brightfield(core, False)
    _label(core, cam, **PAINT)


if __name__ == "__main__":
    from pymmcore_plus import CMMCorePlus

    core = CMMCorePlus()
    if not os.environ.get("ADAPTER_DIR") and not os.environ.get("MM_DIR"):
        sys.exit("Set ADAPTER_DIR to the directory holding the inSiliScope adapter.")
    core.setDeviceAdapterSearchPaths(search_paths())
    t_start = time.time()
    run_checks(core, drift_only="--drift" in sys.argv)  # --drift: the drift checks alone
    print(f"All CellField / XY stage checks passed ({time.time() - t_start:.0f} s).")
