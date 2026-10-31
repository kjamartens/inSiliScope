"""Smoke test of structured illumination in Micro-Manager (pymmcore-plus): the Lasers.Pattern rows and the frame program.

    ADAPTER_DIR=<dir with mmgr_dal_inSiliScope.dll> python tools/test_sim.py

Live frames at 128 px, the microtubules WideField (every dye lit: the fringes show), the 640 nm line; spec/ALGORITHM.md
"Structured illumination", spec/MM_DEVICES.md.
- Layout: the Pattern rows and their defaults (Off); a Pattern pick sets BfpPosition to the type's radius and the
  polarization to S; the Beams matrix shows the type's beams (one row under Off) and takes a row over them.
- The frame program: PatternFrame counts the frames an acquisition takes and restarts at a sequence start
  (PatternRestart Sequence); each frame's metadata tag inSiliScope-PatternFrame is the index it was rendered with
  (0, 1, 2, ... per sequence); PatternState names the state; snaps step it one by one; Never keeps counting.
- The pattern: a 9-frame SIM2D sequence's frames differ between phases, and two sequences from the same state give
  the same frames per state (the program restarted); the 9 frames summed ~ 9 x a WideField frame under Off at the same
  light (the cycle mean: within a few % per block, the sample a little bleached meanwhile).
- The matrix overrides the radius: a one-row edit changes the frame.
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import load_scope, search_paths, timed_prints  # noqa: E402


STATES = []   # the pattern frame index of each popped frame of the last sequence (its metadata)


def sequence(core, n):
    core.startSequenceAcquisition(n, 0, True)
    frames, t0 = [], time.time()
    STATES.clear()
    while len(frames) < n:
        if core.getRemainingImageCount() > 0:
            img, md = core.popNextImageAndMD()
            frames.append(img.astype(np.float64))
            STATES.append(int(md.GetSingleTag("inSiliScope-PatternFrame").GetValue()) if md.HasTag("inSiliScope-PatternFrame") else -1)
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


def fringe(img, period_px, angle_deg):
    """The Fourier component at the fringe's frequency (|F| and its phase) and the median |F| around it."""
    f = img - img.mean()
    F = np.fft.fftshift(np.fft.fft2(f))
    n = f.shape[0]
    c = n // 2
    k = n / period_px
    kx, ky = k * np.cos(np.deg2rad(angle_deg)), k * np.sin(np.deg2rad(angle_deg))
    iy, ix = int(round(c + ky)), int(round(c + kx))
    win = np.abs(F[iy - 2:iy + 3, ix - 2:ix + 3])
    jy, jx = np.unravel_index(np.argmax(win), win.shape)
    py, px = iy - 2 + jy, ix - 2 + jx
    around = np.median(np.abs(F[iy - 8:iy + 9, ix - 8:ix + 9]))
    return abs(F[py, px]), float(np.angle(F[py, px])), float(around), (py, px)


def blocks(img, b=8):
    h, w = img.shape
    return img[:h - h % b, :w - w % b].reshape(h // b, b, w // b, b).mean(axis=(1, 3))


def main():
    from pymmcore_plus import CMMCorePlus

    timed_prints()
    t_start = time.time()
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths())
    load_scope(core, seed=42, fov="128x128")   # TimeWhileIdle Paused, the lasers open, autoshutter off
    # The microtubules WideField: the mode pick applies its light preset (the Typical WideField dye's 488 nm line and
    # filters); 20 ms keeps the bright frames off the camera's ceiling.
    core.setProperty("CellField", "Microtubules_Mode", "WideField")
    core.setProperty("CellField", "Microtubules_Label", "Typical")
    core.setExposure(20)
    core.setProperty("Lasers", "BfpPosition", "0")
    core.setXYPosition("XYStage", 6.58, -5.61)   # a cell's microtubules
    core.waitForDevice("XYStage")

    for p, v in (("Pattern", "Off"), ("PatternPolarization", "S"), ("PatternPhases", 0), ("PatternAngles", 0),
                 ("PatternOrder", "AngleMajor"), ("PatternAngle0Deg", 0), ("PatternPhase0", 0), ("PatternCentreAmp", 1),
                 ("PatternPhaseDriftTurnsPerSqrtSec", 0), ("PatternPhaseDriftTurnsPerSec", 0), ("PatternRestart", "Sequence"),
                 ("PatternState", "no pattern"), ("PatternPeriodNm", -1)):
        got = core.getProperty("Lasers", p)
        assert (float(got) == v) if isinstance(v, (int, float)) else got == v, f"Lasers.{p} = {got}, not {v}"
    rows = core.getProperty("Lasers", "Beams").split(";")
    assert len(rows) == 1 and len(rows[0].split()) == 11, f"Off: one beam row expected, got {rows}"
    assert float(core.getProperty("Objective", "TubeLengthMm")) == 200
    print("Layout OK: the Pattern rows, one beam under Off")

    core.setProperty("Lasers", "Pattern", "SIM2D")
    assert float(core.getProperty("Lasers", "BfpPosition")) == 0.95, "a SIM2D pick sets BfpPosition 0.95"
    assert core.getProperty("Lasers", "PatternPolarization") == "S"
    rows = core.getProperty("Lasers", "Beams").split(";")
    assert len(rows) == 2 and float(rows[0].split()[0]) == 0.95 and float(rows[1].split()[0]) == -0.95, rows
    period = float(core.getProperty("Lasers", "PatternPeriodNm"))
    assert 150 < period < 260, f"SIM2D at 0.95: period {period} nm"
    core.setProperty("Lasers", "Pattern", "LatticeSIM3D")
    assert float(core.getProperty("Lasers", "BfpPosition")) == 0.9
    assert len(core.getProperty("Lasers", "Beams").split(";")) == 5
    print(f"Picks OK: SIM2D period {period:.0f} nm, LatticeSIM3D five beams at 0.9")

    # The frame program (a lower radius: the fringe inside the OTF, a visible pattern). First the plain beam at that
    # radius (the reference the fringe's Fourier peak is compared with, and the cycle check's).
    core.setProperty("Lasers", "Pattern", "Off")
    core.setProperty("Lasers", "BfpPosition", "0.5")
    core.setProperty("Lasers", "State", "0")
    dark = float(np.mean(sequence(core, 2)))
    core.setProperty("Lasers", "State", "1")
    off_frames = sequence(core, 9)
    core.setProperty("Lasers", "Pattern", "SIM2D")
    core.setProperty("Lasers", "BfpPosition", "0.5")
    # Every taken frame counts, the Off ones too (a sequence stop may publish one frame more than was popped): 9 or
    # 10 by now, the state a few frames into the cycle; a sequence start restarts the program.
    pf = int(core.getProperty("Lasers", "PatternFrame"))
    assert pf in (9, 10), pf
    assert core.getProperty("Lasers", "PatternState").startswith(f"angle {pf % 9 // 3} phase {pf % 3}, frame {pf % 9} of 9"), core.getProperty("Lasers", "PatternState")
    a = sequence(core, 9)
    states_a = list(STATES)
    pf = int(core.getProperty("Lasers", "PatternFrame"))
    assert pf in (9, 10), pf
    b = sequence(core, 9)   # restarts at 0
    states_b = list(STATES)
    # Each frame's metadata carries the frame program's index it was rendered with (inSiliScope-PatternFrame): a
    # sequence's frames are 0, 1, 2, ... in order, and the next sequence restarts at 0.
    assert states_a == list(range(9)) and states_b == list(range(9)), f"the frames' pattern indices: {states_a}, then {states_b}"
    # The fringe at 0.5 (period lambda / (2 n u): 365 nm at the label's 488 nm, 3.7 px at the 97.45 nm pixel) in Fourier
    # space: a peak well above its surroundings, its phase stepping by 2 pi / 3 within each orientation, the repeat at
    # the same phase.
    px_nm = float(core.getProperty("Camera", "PixelSizeNm"))   # the sensor pixel over the objective x the relay
    period_px = float(core.getProperty("Lasers", "PatternPeriodNm")) / px_nm   # lambda / (2 n u) at the label's line
    assert 0.0 < a[0].max() < 60000, f"frames saturate ({a[0].max():.0f} ADU): lower the exposure"
    assert 3 < period_px < 6, f"period {period_px:.2f} px"
    # On the difference image (the SIM frame minus twice the Off frame at the same mean light, the dark level
    # kept): the sample's own Fourier component cancels, and the peak is the fringe alone.
    peaks, phases, phases_b = [], [], []
    for k in range(9):
        diff_a, diff_b = a[k] - 2 * off_frames[k] + dark, b[k] - 2 * off_frames[k] + dark
        amp, ph, around, at = fringe(diff_a, period_px, 60 * (k // 3))
        amp_b, ph_b, _, at_b = fringe(diff_b, period_px, 60 * (k // 3))
        amp_off, _, _, _ = fringe(off_frames[k], period_px, 60 * (k // 3))
        assert amp > 2 * around, f"frame {k}: no fringe peak ({amp:.0f} vs {around:.0f} around, {amp_off:.0f} under Off)"
        peaks.append(amp / around)
        phases.append(ph)
        phases_b.append(ph_b)
    wrap = lambda x: (x + np.pi) % (2 * np.pi) - np.pi
    steps = [abs(wrap(phases[k + 1] - phases[k])) for k in range(8) if (k + 1) % 3]
    assert all(abs(st - 2 * np.pi / 3) < 0.6 for st in steps), f"phase steps {np.round(steps, 2)} (expected 2.09)"
    same = [abs(wrap(phases_b[k] - phases[k])) for k in range(9)]
    assert max(same) < 0.4, f"a second sequence should repeat the phases: {np.round(same, 2)}"
    print(f"Program OK: the frames' pattern indices 0..8 twice (metadata), fringe peaks {min(peaks):.0f}-{max(peaks):.0f}x their "
          f"surroundings, phase steps {np.mean(steps):.2f} rad (2.09), the repeat within {max(same):.2f} rad")
    same = [float(np.abs(a[k] - b[k]).mean()) for k in range(9)]
    # Snaps step it one by one; Never keeps counting across a sequence start.
    pf = int(core.getProperty("Lasers", "PatternFrame"))
    snap(core)
    snap(core)
    assert int(core.getProperty("Lasers", "PatternFrame")) == pf + 2, (pf, core.getProperty("Lasers", "PatternFrame"))
    assert core.getProperty("Lasers", "PatternState").startswith(f"angle {(pf + 2) % 9 // 3} phase {(pf + 2) % 3}")
    core.setProperty("Lasers", "PatternRestart", "Never")
    sequence(core, 3)
    assert int(core.getProperty("Lasers", "PatternFrame")) in (pf + 5, pf + 6), (pf, core.getProperty("Lasers", "PatternFrame"))
    core.setProperty("Lasers", "PatternRestart", "Sequence")
    print("Counter OK: snaps count one by one, Never carries the count across a sequence")

    # The cycle mean: the 9 frames' sum against 9 x the Off frame at the same mean light. Off's beam at 0.5 is one beam
    # at F x the envelope; the SIM2D pair carries the two beams' power each at the laser power, so the mean is 2 x
    # (the mean field at the beams' mean): compare the summed frames to 2 x 9 x the Off frame per 8 x 8 block,
    # the dark level removed.
    s9 = sum(sequence(core, 9)) - 9 * dark
    off = sum(off_frames) - 9 * dark
    bs, bo = blocks(s9), blocks(off)
    lit = bo > 0.1 * bo.max()
    ratio = bs[lit] / (2 * bo[lit])
    assert 0.9 < np.median(ratio) < 1.1 and np.std(ratio) < 0.1, f"cycle mean: ratio {np.median(ratio):.3f} +- {np.std(ratio):.3f}"
    print(f"Cycle OK: the 9 SIM frames summed = 2 x 9 x the Off frames per block ({np.median(ratio):.3f} +- {np.std(ratio):.3f})")

    # The matrix overrides the radius: both beams at 0.3 (a wider fringe) change the frame.
    core.setProperty("Lasers", "Pattern", "SIM2D")
    core.setProperty("Lasers", "BfpPosition", "0.5")
    core.startSequenceAcquisition(1, 0, True)
    core.stopSequenceAcquisition()
    f1 = snap(core)
    core.setProperty("Lasers", "Beams", "0.3 0 0 0 0 1 0 0 0 1 0; -0.3 0 0 0 0 1 0 0 0 -1 0")
    assert core.getProperty("Lasers", "Beams").split(";")[0].split()[0] == "0.3"
    core.startSequenceAcquisition(1, 0, True)
    core.stopSequenceAcquisition()
    f2 = snap(core)
    assert float(np.abs(f1 - f2).mean()) > 2 * np.mean(same), "the matrix's radius should change the frame"
    period2 = float(core.getProperty("Lasers", "PatternPeriodNm"))
    assert abs(period2 / period - 0.95 / 0.3) < 0.05, f"period {period2:.0f} vs {period:.0f} x 0.95 / 0.3"
    try:
        core.setProperty("Lasers", "Beams", "0.3 x")
        sys.exit("a bad matrix was accepted")
    except Exception:
        pass
    print(f"Matrix OK: two beams at 0.3 (period {period2:.0f} nm)")
    print(f"test_sim OK in {time.time() - t_start:.1f} s")


if __name__ == "__main__":
    main()
