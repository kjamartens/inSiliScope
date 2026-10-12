"""Smoke test of the specimens in Micro-Manager (pymmcore-plus): SampleHolder mounts the cell field or the nanorulers.

    ADAPTER_DIR=<dir with mmgr_dal_inSiliScope.dll> python tools/test_specimens.py

Precomputed 10-frame stacks at 128 px (Camera Test rows, ISC_TEST=1); docs/samples/index.md, spec/MM_DEVICES.md.
- SampleHolder: its positions are data/specimens.json's (CellField, Nanorulers), CellField mounted at start.
- The Nanorulers device: its label (Docking_*), design and strands rows; Fluorophores resolves the docking strands'
  Typical label in DNA-PAINT to ATTO 655.
- Nanorulers mounted, focus on the docking strands (0.02 um), Fluorophores Mode dSTORM (the docking strands' Typical
  label follows: AF647, and its light preset): a stack shows bright spots (a fresh place: every dye in its initial ON
  phase); with no docking strands labelled (LabelingPct 0) only noise is left; 1 strand per spot instead of 5 is a new
  world (another stack). (DNA-PAINT at the default 1 nM imager gives a few blinks per hundred frames over this field.)
- Live and BrightField frames of the nanorulers render (BrightField: no contrast, DNA this thin is invisible).
- Back to CellField at the old focus: its stack is the first one, pixel for pixel (a specimen switch is a new world:
  the illumination history starts afresh, nothing of the nanorulers is left).
"""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import brightfield, load_scope, search_paths, timed_prints, wait_for_stack  # noqa: E402

N = 10


def stack(core):
    """The N frames of a fresh precomputed stack, as float arrays."""
    core.setProperty("Camera", "Test_GenerateStack", "1")
    wait_for_stack(core)
    frames = []
    for _ in range(N):
        core.snapImage()
        frames.append(core.getImage().astype(np.float64))
    return frames


def focus(core, um):
    core.setPosition("ZStage", um)
    core.waitForDevice("ZStage")


def spots(frames):
    """Bright, sparse spots in the frames' sum: its 99.99th percentile above the median, in robust sigmas."""
    s = np.sum(frames, axis=0)
    med = float(np.median(s))
    sig = 1.4826 * float(np.median(np.abs(s - med))) + 1e-9
    return (float(np.percentile(s, 99.99)) - med) / sig


def main():
    from pymmcore_plus import CMMCorePlus

    timed_prints()
    t_start = time.time()
    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths(require_mm=False))
    load_scope(core, detail="Expert")
    core.setProperty("Camera", "Test_StackLength", str(N))
    core.setProperty("Camera", "Test_AcqMode", "Precomputed")
    core.setExposure(50.0)

    labels = list(core.getStateLabels("SampleHolder"))
    assert labels == ["CellField", "Nanorulers"], f"SampleHolder positions: {labels}"
    assert core.getProperty("SampleHolder", "Label") == "CellField", "the cell field should be mounted at start"
    for p, v in (("Docking_Label", "Typical"), ("Design", "Nanoruler80"), ("StrandsPerSpot", 5),
                 ("PersistenceLengthNm", 1500), ("Bending", "On")):
        got = core.getProperty("Nanorulers", p)
        assert (float(got) == v) if isinstance(v, int) else got == v, f"Nanorulers.{p} = {got}, not {v}"
    eff = core.getProperty("Fluorophores", "Docking_EffectiveDye")
    assert eff == "ATTO655 (DNA-PAINT)", f"the docking strands' Typical label: {eff}"
    print(f"Layout OK: SampleHolder {labels}, Nanorulers rows, docking label {eff}")

    z0 = core.getPosition("ZStage")
    cells = stack(core)
    assert any(f.std() > 3 for f in cells), "precomputed CellField frames look empty"

    core.setProperty("SampleHolder", "Label", "Nanorulers")
    assert core.getProperty("SampleHolder", "State") == "1"
    focus(core, 0.02)
    core.setProperty("Fluorophores", "Mode", "dSTORM")
    eff = core.getProperty("Fluorophores", "Docking_EffectiveDye")
    assert eff == "AF647 (dSTORM)", f"the docking strands' Typical label in dSTORM: {eff}"
    assert core.getProperty("Lasers", "Preset") == "dSTORM-640", core.getProperty("Lasers", "Preset")
    rulers = stack(core)
    core.setProperty("Nanorulers", "Docking_LabelingPct", "0")
    empty = stack(core)
    core.setProperty("Nanorulers", "Docking_LabelingPct", "80")
    s_rulers, s_empty = spots(rulers), spots(empty)
    assert s_rulers > 20 and s_empty < 8, f"nanoruler spots: {s_rulers:.1f} sigma with docking strands, {s_empty:.1f} without"
    print(f"Nanorulers OK: dSTORM spots at {s_rulers:.0f} sigma over 10 frames ({s_empty:.1f} with no strands)")

    core.setProperty("Nanorulers", "StrandsPerSpot", "1")
    one = stack(core)
    assert not all(np.array_equal(a, b) for a, b in zip(one, rulers)), "1 strand per spot should be another sample"
    assert spots(one) > 8, f"1 strand per spot: {spots(one):.1f} sigma"
    core.setProperty("Nanorulers", "StrandsPerSpot", "5")
    core.setProperty("Fluorophores", "Mode", "DNA-PAINT")
    assert core.getProperty("Lasers", "Preset") == "PAINT-640", core.getProperty("Lasers", "Preset")
    print(f"StrandsPerSpot OK: 1 strand per spot is another sample ({spots(one):.0f} sigma)")

    core.setProperty("Camera", "Test_AcqMode", "Live")
    core.snapImage()
    live = core.getImage().astype(np.float64)
    assert live.shape == cells[0].shape and live.std() > 0
    brightfield(core, True)
    core.snapImage()
    bf = core.getImage().astype(np.float64)
    brightfield(core, False)
    # Shot noise of the lamp alone: the frame's spread is ~sqrt(photons)/gain, no structure (the frame's mean of each
    # 16 x 16 block stays within a few % of the whole frame's).
    blocks = bf.reshape(8, 16, 8, 16).mean(axis=(1, 3))
    assert bf.mean() > 1000 and np.ptp(blocks) < 0.05 * (bf.mean() - 100), \
        f"BrightField nanorulers should be flat: block means {blocks.min():.1f}..{blocks.max():.1f}"
    core.setProperty("Camera", "Test_AcqMode", "Precomputed")
    print(f"Live and BrightField OK: a live frame, a flat BrightField frame (mean {bf.mean():.0f} ADU)")

    core.setProperty("SampleHolder", "State", "0")
    focus(core, z0)
    again = stack(core)
    assert all(np.array_equal(a, b) for a, b in zip(again, cells)), \
        "CellField again should be the first stack (a new world: the illumination history starts afresh)"
    print("CellField again OK: its stack is the first one, pixel for pixel")
    print(f"All specimen checks passed in {time.time() - t_start:.1f} s")


if __name__ == "__main__":
    main()
