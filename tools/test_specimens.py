"""Smoke test of the specimens in Micro-Manager (pymmcore-plus): SampleHolder mounts the cell field, the nanorulers,
the bead sample or the fluorescent slide.

    ADAPTER_DIR=<dir with mmgr_dal_inSiliScope.dll> python tools/test_specimens.py

Precomputed 10-frame stacks at 128 px (Camera Test rows, ISC_TEST=1); docs/samples/index.md, spec/MM_DEVICES.md.
- SampleHolder: its positions are data/specimens.json's (CellField, Nanorulers, Beads, Slide), CellField mounted at start.
- The Nanorulers device: its label (Docking_*), design and strands rows; Fluorophores resolves the docking strands'
  Typical label in DNA-PAINT to ATTO 655.
- Nanorulers mounted, focus on the docking strands (0.02 um), Docking_Mode dSTORM (the docking strands' Typical
  label follows: AF647, and the Lasers' Auto preset = its dSTORM-640): a stack shows bright spots (a fresh place: every dye in its initial ON
  phase); with no docking strands labelled (LabelingPct 0) only noise is left; 1 strand per spot instead of 5 is a new
  world (another stack). (DNA-PAINT at the default 1 nM imager gives a few blinks per hundred frames over this field.)
- Live and BrightField frames of the nanorulers render (BrightField: no contrast, DNA this thin is invisible).
- Back to CellField at the old focus: its stack is the first one, pixel for pixel (a specimen switch is a new world:
  the illumination history starts afresh, nothing of the nanorulers is left).
- The bead sample: mounting it with no beads sets Fiducials.DensityPerUm2 to 0.3 and AggregationPct to 40, moves OnTop
  beads to Coverslip and offers no OnTop; the Lasers' Auto preset is the bead sample's (561 nm at 0.1 kW/cm^2, the
  others off); focused on the beads a stack shows spots; AggregationPct 0 is another sample (the same number of beads,
  single); a Type pick sets DiameterNm to its product size; a density set by hand stays at a mount; back on the cell
  field OnTop is offered again.
- The fluorescent slide: the FluorescentSlide device's defaults (5 um, Mix, 10 uM, 900 s); the Lasers' Auto preset is
  its Slide-561 (561 nm at 0.02 kW/cm^2, the others off); a stack is bright and smooth (no spots), its signal linear in
  the concentration; a short half-time bleaches it from frame to frame; a Dye pick of AF647 sets 640 nm at 0.02.
"""

import os
import sys
import json
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import brightfield, load_scope, search_paths, timed_prints, wait_for_stack  # noqa: E402

N = 10
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "dyes")
TYPICAL = json.load(open(os.path.join(DATA, "library.json"), encoding="utf8"))["typicalLabels"]["docking"]


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
    assert labels == ["CellField", "Nanorulers", "Beads", "Slide"], f"SampleHolder positions: {labels}"
    assert core.getProperty("SampleHolder", "Label") == "CellField", "the cell field should be mounted at start"
    for p in ("Docking_Label", "Design", "SpacingNm", "StrandsPerSpot", "PersistenceLengthNm", "Bending"):
        assert core.hasProperty("Nanorulers", p), f"missing Nanorulers.{p}"
    assert core.getProperty("Nanorulers", "Docking_Label") == "Typical"
    mode = core.getProperty("Nanorulers", "Docking_Mode")
    eff = core.getProperty("Fluorophores", "Docking_EffectiveDye")
    assert eff == f"{TYPICAL[mode]['dye']} ({mode})", f"the docking strands' Typical label: {eff} (data: {TYPICAL[mode]})"
    print(f"Layout OK: SampleHolder {labels}, Nanorulers rows, docking label {eff}")

    z0 = core.getPosition("ZStage")
    cells = stack(core)
    assert any(f.std() > 3 for f in cells), "precomputed CellField frames look empty"

    core.setProperty("SampleHolder", "Label", "Nanorulers")
    assert core.getProperty("SampleHolder", "State") == "1"
    focus(core, 0.02)
    core.setProperty("Nanorulers", "Docking_Mode", "dSTORM")
    eff = core.getProperty("Fluorophores", "Docking_EffectiveDye")
    assert eff == f"{TYPICAL['dSTORM']['dye']} (dSTORM)", f"the docking strands' Typical label in dSTORM: {eff}"
    dstorm_cube = core.getProperty("FilterCube", "Label")
    assert core.getProperty("Lasers", "Preset") == "Auto", core.getProperty("Lasers", "Preset")
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
    core.setProperty("Nanorulers", "Docking_Mode", "DNA-PAINT")
    assert core.getProperty("FilterCube", "Label") != dstorm_cube, "back in DNA-PAINT: not the dSTORM preset's cube"
    print(f"StrandsPerSpot OK: 1 strand per spot is another sample ({spots(one):.0f} sigma)")

    # The site spacing and the 2D origami: each is another sample, and the spots are still there.
    core.setProperty("Nanorulers", "Docking_Mode", "dSTORM")
    allowed = list(core.getAllowedPropertyValues("Nanorulers", "SpacingNm"))
    assert sorted(map(int, allowed)) == [5, 10, 20, 40, 80, 160, 320, 640], f"SpacingNm values: {allowed}"
    core.setProperty("Nanorulers", "SpacingNm", "320")
    wide = stack(core)
    assert not all(np.array_equal(a, b) for a, b in zip(wide, rulers)), "320 nm spacing should be another sample"
    assert spots(wide) > 8, f"320 nm spacing: {spots(wide):.1f} sigma"
    core.setProperty("Nanorulers", "Design", "TUDelftLogo")
    core.setProperty("Nanorulers", "SpacingNm", "40")
    core.setProperty("Nanorulers", "DensityPerUm2", "0.2")   # sparse logos: spots() wants sparse spots
    logo = stack(core)
    core.setProperty("Nanorulers", "DensityPerUm2", "1.2")
    assert not all(np.array_equal(a, b) for a, b in zip(logo, wide)), "the 2D origami should be another sample"
    assert spots(logo) > 8, f"2D origami: {spots(logo):.1f} sigma"
    core.setProperty("Nanorulers", "Design", "Nanoruler")
    core.setProperty("Nanorulers", "SpacingNm", "80")
    core.setProperty("Nanorulers", "Docking_Mode", "DNA-PAINT")
    print(f"SpacingNm and Design OK: 320 nm rods ({spots(wide):.0f} sigma), TU Delft logos ({spots(logo):.0f} sigma)")

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
    # A mount keeps the lasers as set (the Nanorulers' DNA-PAINT preset set TIRF): back to the CellField's Auto light.
    core.setProperty("Lasers", "Preset", "Auto")
    again = stack(core)
    assert all(np.array_equal(a, b) for a, b in zip(again, cells)), \
        "CellField again should be the first stack (a new world: the illumination history starts afresh)"
    print("CellField again OK: its stack is the first one, pixel for pixel")

    # The bead sample: no targets, only the fiducial beads (gold by default).
    assert float(core.getProperty("Fiducials", "DensityPerUm2")) == 0
    core.setProperty("Fiducials", "Height", "OnTop")
    core.setProperty("SampleHolder", "Label", "Beads")
    dens, agg = float(core.getProperty("Fiducials", "DensityPerUm2")), float(core.getProperty("Fiducials", "AggregationPct"))
    assert dens == 0.3 and agg == 40, f"mounting the bead sample should set 0.3 beads/um^2 and 40 % aggregation: {dens}, {agg}"
    heights = list(core.getAllowedPropertyValues("Fiducials", "Height"))
    assert core.getProperty("Fiducials", "Height") == "Coverslip" and "OnTop" not in heights, \
        f"the bead sample: OnTop beads on the glass, no OnTop offered ({core.getProperty('Fiducials', 'Height')}, {heights})"
    core.setProperty("Lasers", "Preset", "Auto")
    lasers = {nm: float(core.getProperty("Lasers", f"Laser{nm}KWcm2")) for nm in (405, 488, 561, 640)}
    assert lasers == {405: 0, 488: 0, 561: 0.1, 640: 0}, f"the bead sample's light (561 nm at 0.1 kW/cm^2): {lasers}"
    focus(core, 0.05)
    beads = stack(core)
    s_beads = spots(beads)
    assert s_beads > 8, f"bead sample: {s_beads:.1f} sigma"
    core.setProperty("Fiducials", "AggregationPct", "0")
    single = stack(core)
    assert not all(np.array_equal(a, b) for a, b in zip(single, beads)), "AggregationPct 0 should be another sample"
    for t, d in (("FluoSpheresRed", 200), ("Gold", 100)):
        core.setProperty("Fiducials", "Type", t)
        got = float(core.getProperty("Fiducials", "DiameterNm"))
        assert got == d, f"a {t} pick should set DiameterNm {d}, got {got}"
    core.setProperty("SampleHolder", "State", "0")
    assert "OnTop" in core.getAllowedPropertyValues("Fiducials", "Height"), "the cell field offers OnTop again"
    core.setProperty("Fiducials", "DensityPerUm2", "0.05")
    core.setProperty("SampleHolder", "Label", "Beads")
    assert float(core.getProperty("Fiducials", "DensityPerUm2")) == 0.05, "a density set by hand should stay at a mount"
    core.setProperty("Fiducials", "DensityPerUm2", "0")
    core.setProperty("Fiducials", "AggregationPct", "0")
    core.setProperty("SampleHolder", "State", "0")
    print(f"Bead sample OK: 0.3 beads/um^2 and 40 % at the mount, Coverslip, 561 nm at 0.1 kW/cm^2, spots at "
          f"{s_beads:.0f} sigma, single beads another sample, the diameter follows the type")

    # The fluorescent slide: a uniform dye layer, its light the engine's.
    core.setProperty("SampleHolder", "Label", "Slide")
    rows = {k: core.getProperty("FluorescentSlide", k)
            for k in ("ThicknessUm", "Dye", "ConcentrationMicroMolar", "BleachHalfTimeSec")}
    assert (float(rows["ThicknessUm"]), rows["Dye"], float(rows["ConcentrationMicroMolar"]),
            float(rows["BleachHalfTimeSec"])) == (5, "Mix", 10, 900), f"the slide's defaults: {rows}"
    core.setProperty("Lasers", "Preset", "Auto")
    lasers = {nm: float(core.getProperty("Lasers", f"Laser{nm}KWcm2")) for nm in (405, 488, 561, 640)}
    assert lasers == {405: 0, 488: 0, 561: 0.02, 640: 0}, f"the slide's light (561 nm at 0.02 kW/cm^2): {lasers}"
    focus(core, 2.0)
    offset = float(core.getProperty("Camera", "OffsetADU"))
    c = slice(54, 74)   # the beam's centre
    full = np.mean(stack(core), axis=0) - offset
    s_slide = spots(stack(core))
    assert full[c, c].mean() > 10000 and s_slide < 8, \
        f"the slide should be bright and smooth: {full[c, c].mean():.0f} ADU, spots at {s_slide:.1f} sigma"
    core.setProperty("FluorescentSlide", "ConcentrationMicroMolar", "5")
    half = np.mean(stack(core), axis=0) - offset
    ratio = half[c, c].mean() / full[c, c].mean()
    assert abs(ratio - 0.5) < 0.01, f"the signal should be linear in the concentration: 5 / 10 uM = {ratio:.4f}"
    core.setProperty("FluorescentSlide", "BleachHalfTimeSec", "0.01")
    frames = stack(core)
    drop = (frames[-1][c, c].mean() - offset) / (frames[0][c, c].mean() - offset)
    assert drop < 0.9, f"a 0.01 s half-time should bleach the slide within {N} frames: last / first {drop:.3f}"
    core.setProperty("FluorescentSlide", "Dye", "AF647")
    lasers = {nm: float(core.getProperty("Lasers", f"Laser{nm}KWcm2")) for nm in (405, 488, 561, 640)}
    assert lasers == {405: 0, 488: 0, 561: 0, 640: 0.02}, f"an AF647 pick should set 640 nm at 0.02 kW/cm^2: {lasers}"
    for k, v in rows.items():
        core.setProperty("FluorescentSlide", k, v)
    core.setProperty("SampleHolder", "State", "0")
    print(f"Fluorescent slide OK: defaults, Slide-561, {full[c, c].mean():.0f} ADU at the centre (spots "
          f"{s_slide:.1f} sigma), 5 / 10 uM = {ratio:.4f}, bleached to {drop:.2f} in {N} frames, AF647 -> 640 nm")
    print(f"All specimen checks passed in {time.time() - t_start:.1f} s")


if __name__ == "__main__":
    main()
