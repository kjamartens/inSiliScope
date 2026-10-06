"""Smoke test for the inSiliScope device adapter (the hub and its devices), using pymmcore-plus.

Exercises: the device layout and the property tiers (Detail Basic / Advanced / Expert; the Test rows only with
ISC_TEST=1), pre-init enforcement (the hub's RandomSeed and Detail, the camera's FovSize), background stack generation,
reproducibility (same seed + settings -> identical precomputed stack), pixel shape/dtype, live mode end-to-end,
couplings across devices (a dye pick loading its light preset and filter cube, a camera preset its noise values and
sensor pixel, the pixel size from sensor / objective / emission path), the light sources' shutters (none open: dark
frames), and that changing a noise parameter while live actually changes subsequent frames. Then the photophysics,
EMCCD, illumination/background and GPU checks.

Requires an MM nightly build installed (e.g. via `mmcore install`) whose device-interface version matches the
checked-out mmCoreAndDevices submodule commit; ADAPTER_DIR (e.g. adapter/inSiliScope/build/Release/x64) is searched
first. See docs/dev/BUILD_AND_USAGE.md. Under 5 minutes.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isc_mm import (ALL_DEVICES, brightfield, custom_microtubule_dye, generate_stack, load_scope, precomputed,  # noqa: E402
                    search_paths, set_light, timed_prints)

import numpy as np  # noqa: E402
from pymmcore_plus import CMMCorePlus  # noqa: E402

timed_prints()
t_start = time.time()

core = CMMCorePlus()
core.setDeviceAdapterSearchPaths(search_paths())


def fresh(seed=42, **kw):
    """A fresh scope (a fresh hub: no illumination history), 200-frame stacks (the most any check reads)."""
    core.unloadAllDevices()
    load_scope(core, seed=seed, **kw)
    assert int(core.getProperty("Camera", "Test_StackLength")) == 1000, "Test_StackLength default should be 1000"
    core.setProperty("Camera", "Test_StackLength", "200")


def snaps(n):
    out = []
    for _ in range(n):
        core.snapImage()
        out.append(core.getImage().astype(np.float64))
    return out


# --- device layout, pre-init, tiers ---------------------------------------
fresh()
for label, prop in (("Hub", "RandomSeed"), ("Hub", "Detail"), ("Camera", "FovSize")):
    assert core.isPropertyPreInit(label, prop), f"{label}.{prop} should be pre-init"
hub_children = set(core.getInstalledDevices("Hub"))
assert set(ALL_DEVICES) <= hub_children, f"the hub should offer every device (Expert): missing {set(ALL_DEVICES) - hub_children}"
assert "1024x1024 (slow, ~4x the memory of 512)" in core.getAllowedPropertyValues("Camera", "FovSize")
print("Layout OK: hub + %d devices, pre-init RandomSeed/Detail/FovSize" % len(ALL_DEVICES))

STANDARD = {"Name", "Description", "HubID", "State", "Label", "Position", "Exposure", "Binning", "PixelType",
            "CameraName", "CameraID", "TransposeMirrorX", "TransposeMirrorY", "TransposeXY", "TransposeCorrection"}


def own_props(devices):
    return {(d, p) for d in devices for p in core.getDevicePropertyNames(d)
            if p not in STANDARD and not core.isPropertyPreInit(d, p) and not p.startswith("Test_")}


expert = own_props(ALL_DEVICES)
core.unloadAllDevices()
load_scope(core, detail="Basic", devices=[d for d in ALL_DEVICES if d not in ("Dichroic", "EmissionFilter")])
basic = own_props([d for d in ALL_DEVICES if d not in ("Dichroic", "EmissionFilter")])
assert "Dichroic" not in core.getInstalledDevices("Hub"), "Basic: the hub should not offer the Advanced filter wheels"
assert basic < expert, "Basic should show a subset of Expert"
assert len(basic) <= 30, f"Basic shows {len(basic)} properties (cap 30): {sorted(basic)}"
for must in (("Camera", "CameraPreset"), ("Lasers", "Laser640KWcm2"), ("Lasers", "Preset"), ("CellField", "Microtubules_Dye"),
             ("Renderer", "Quality"), ("TransmittedLamp", "IntensityPhotonsPerPxPerSec")):
    assert must in basic, f"{must} should be Basic"
for hidden in (("Objective", "NA"), ("Renderer", "PsfOversampling"), ("Camera", "GainElectronsPerADU")):
    assert hidden not in basic and hidden in expert, f"{hidden} should be above Basic"
print(f"Tiers OK: Basic shows {len(basic)} properties, Expert {len(expert)}")
fresh()

# --- precomputed stack (Test rows): generate, snap -------------------------
precomputed(core)
generate_stack(core)
print("Stack generation status:", core.getProperty("Camera", "Test_StackGenerationStatus"))
core.snapImage()
img = core.getImage()
assert img.shape == (128, 128), f"expected (128, 128), got {img.shape}"
assert img.dtype == np.uint16, f"expected uint16, got {img.dtype}"
assert img.std() > 0, "expected a non-blank frame"
print("Snap OK. Image shape:", img.shape, "dtype:", img.dtype, "std:", img.std())

# --- reproducibility: same seed + settings -> byte-identical stack ----------
frames_a = [img.astype(np.float64)] + snaps(4)
fresh(seed=42)
precomputed(core)
generate_stack(core)
frames_b = snaps(5)
for i, (a, b) in enumerate(zip(frames_a, frames_b)):
    assert np.array_equal(a, b), f"frame {i} differs between two runs with the same seed"
fresh(seed=43)
precomputed(core)
generate_stack(core)
assert not np.array_equal(snaps(1)[0], frames_a[0]), "a different RandomSeed should give a different frame"
print("Reproducibility OK: same seed -> identical frames, another seed -> another frame")

# --- live mode -------------------------------------------------------------
precomputed(core, False)
core.startSequenceAcquisition(10, 20.0, True)
while core.isSequenceRunning():
    time.sleep(0.02)
time.sleep(0.2)
live_frames = []
while core.getRemainingImageCount() > 0:
    live_frames.append(core.popNextImage())
assert len(live_frames) >= 2, f"expected several live frames, got {len(live_frames)}"
assert not np.array_equal(live_frames[0], live_frames[-1]), "expected live frames to differ over time"
print("Live mode OK:", len(live_frames), "frames captured, frames vary over time")

# --- defaults and couplings across devices ----------------------------------
fresh()
for (dev, name), expected in {
    ("Lasers", "IlluminationProfile"): "Flat",
    ("CellField", "Microtubules_Dye"): "ATTO655",
    ("CellField", "Microtubules_Mode"): "DyeDefault",
    ("CellField", "Microtubules_LabelingPct"): "70",
    ("Lasers", "Preset"): "PAINT-640",
    ("FilterCube", "Label"): "LP650+BP676-37",
    ("Objective", "Label"): "100x/1.40 Oil",
    ("Camera", "CameraType"): "sCMOS",
    ("Camera", "PixelSizeNm"): "100",
    ("SampleHolder", "BackgroundDecaySec"): "0",
    ("Renderer", "UseGpu"): "On",
    ("Renderer", "DiskCache"): "Cells",
    ("Renderer", "PsfInterp"): "Cubic",
    ("Renderer", "PsfModel"): "GibsonLanniZernike",
    ("Renderer", "PsfOversampling"): "6",
    ("Renderer", "Quality"): "Realistic",
    ("Objective", "ZernikePreset"): "MixedRealisticObjective",
    ("Objective", "PsfKernelHalfWidthNm"): "7000",
}.items():
    actual = core.getProperty(dev, name)
    ok = float(actual) == float(expected) if expected.replace(".", "").isdigit() else actual == expected
    assert ok, f"expected {dev}.{name} to default to {expected!r}, got {actual!r}"
# A dye pick loads its mode's light preset: lasers and filter cube on their own devices.
core.setProperty("CellField", "Microtubules_Dye", "AF647")
assert core.getProperty("Lasers", "Preset") == "dSTORM-640", core.getProperty("Lasers", "Preset")
assert core.getProperty("FilterCube", "Label") == "LP650+ChromaET700-75m", core.getProperty("FilterCube", "Label")
assert float(core.getProperty("CellField", "Microtubules_LabelingPct")) == 3, "a mode change sets its labelling"
core.setProperty("Dichroic", "Label", "Quad")
assert core.getProperty("FilterCube", "Label") == "Custom", "a wheel set by hand makes the cube Custom"
assert core.getProperty("Lasers", "Preset") == "None", "a filter set by hand clears the light preset"
core.setProperty("FilterCube", "Label", "LP570+BP617-73")
assert core.getProperty("Dichroic", "Label") == "LP570" and core.getProperty("EmissionFilter", "Label") == "BP617-73"
# A camera preset: noise values, sensor pixel (the pixel size follows), sensor type.
core.setProperty("Camera", "CameraPreset", "iXonUltra897")
assert core.getProperty("Camera", "CameraType") == "EMCCD" and float(core.getProperty("Camera", "SensorPixelUm")) == 16
assert abs(float(core.getProperty("Camera", "PixelSizeNm")) - 16000 / 65) < 1e-3, core.getProperty("Camera", "PixelSizeNm")
core.setProperty("EmissionPath", "EmissionMagnification", "1.6")
core.setProperty("Objective", "Label", "60x/1.20 Water")
assert float(core.getProperty("Objective", "NA")) == 1.2 and abs(float(core.getProperty("Camera", "PixelSizeNm")) - 16000 / 96) < 1e-3
core.setProperty("Objective", "NA", "1.1")
assert core.getProperty("Objective", "Label") == "Custom", "an NA set by hand makes the turret position Custom"
core.setProperty("Renderer", "PsfOversampling", "4")
assert core.getProperty("Renderer", "Quality") == "Custom"
core.setProperty("Renderer", "Quality", "Realistic")
assert core.getProperty("Renderer", "PsfOversampling") == "6"
print("Defaults and couplings OK: dye -> light preset + cube, wheels <-> cube, camera preset -> sensor pixel, "
      "objective/emission path -> pixel size, quality <-> numerics")

# --- the light: no shutter open gives dark frames ----------------------------
fresh()
precomputed(core)
set_light(core, False, False)
generate_stack(core)
dark = np.stack(snaps(5))
assert abs(dark.mean() - 100.0) < 1.0, f"no light: dark frames around the offset, got mean {dark.mean():.2f}"
set_light(core, True, False)
generate_stack(core)
lit = np.stack(snaps(5))
assert lit.mean() > dark.mean() + 1.0, "the lasers' shutter open should light the sample"
print(f"Shutters OK: dark {dark.mean():.2f} ADU with no light, {lit.mean():.2f} ADU with the lasers open")

# --- labelling: fewer labelled sites, less light -----------------------------
# dSTORM AF647 (no imager background); the initial ON burst switched off in a custom dye (InitialOnSec 0). Each
# stack on a fresh scope: a stack continues the illumination history of its spot.
FAST = [("Renderer", "PsfModel", "Gaussian"), ("SampleHolder", "BackgroundPhotonsPerSec", "0")]


def stack_frames(settings, n=20, seed=42, skip=0, dye_edits=None):
    """Fresh scope, apply settings ((device, prop, value) list) and dye edits, return n stack frames after skip."""
    fresh(seed=seed)
    precomputed(core)
    for dev, p, v in settings:
        core.setProperty(dev, p, v)
    if dye_edits:
        custom_microtubule_dye(core, **dye_edits)
    generate_stack(core)
    snaps(skip)
    return np.stack(snaps(n))


def mean_signal(pct):
    fr = stack_frames(FAST + [("CellField", "Microtubules_Dye", "AF647"),
                              ("CellField", "Microtubules_LabelingPct", str(pct))], n=50,
                      dye_edits={"InitialOnSec": 0})
    return fr.mean() - 100.0


sig_full, sig_low = mean_signal(70), mean_signal(7)
assert sig_low < 0.3 * sig_full, f"expected 7% labelling to give ~1/10 of the 70% signal ({sig_low:.3f} vs {sig_full:.3f})"
print(f"Labelling OK: mean signal {sig_full:.3f} -> {sig_low:.3f} ADU from 70% to 7% labelled sites")

# --- live responsiveness: nothing heavy runs per frame -----------------------
fresh()
core.setProperty("Renderer", "PsfModel", "Gaussian")
core.setProperty("Camera", "Exposure", "20")
for _ in range(2):   # the first frames build the world and prefetch around the FOV
    core.startSequenceAcquisition(20, 20.0, True)
    while core.isSequenceRunning():
        time.sleep(0.02)
    time.sleep(0.2)
    while core.getRemainingImageCount() > 0:
        core.popNextImage()
interval_ms = float(core.getProperty("Camera", "ActualFrameIntervalMs"))
assert interval_ms < 100.0, f"expected ActualFrameIntervalMs within ~5x of the 20 ms exposure, got {interval_ms} ms"
print(f"Live responsiveness OK: ActualFrameIntervalMs={interval_ms:.1f} ms streaming live")

# --- live: a noise parameter changes subsequent frames ------------------------
core.setProperty("Camera", "OffsetStdADU", "0.0")
core.setProperty("SampleHolder", "BackgroundPhotonsPerSec", "0.0")
core.setProperty("CellField", "Microtubules_LabelingPct", "0")   # noise only: no dyes, no imager background
core.setProperty("CellField", "Microtubules_ImagerNm", "0")
time.sleep(0.3)
std_before = float(np.std(snaps(1)[0]))
core.setProperty("Camera", "OffsetStdADU", "50.0")
time.sleep(0.3)
std_after = float(np.std(snaps(1)[0]))
assert std_after > std_before + 10.0, f"OffsetStdADU=50 live should raise the pixel std ({std_before:.3f} -> {std_after:.3f})"
print(f"Live settings OK: OffsetStdADU change took effect live (std {std_before:.2f} -> {std_after:.2f})")

# --- PsfInterp and Zernike presets -------------------------------------------
fresh()
precomputed(core)
for dev, p, v in (("Objective", "ZernikePreset", "AstigmatismModerate"), ("Objective", "PsfZRangeUm", "2"),
                  ("Objective", "PsfZStepUm", "0.2"), ("CellField", "Microtubules_LabelingPct", "0.02")):
    core.setProperty(dev, p, v)   # sparse labelling: Fft placement is one Fourier shift per emitter
INTERP_VALUES = {"Nearest", "Linear", "Cubic", "Fft"}
assert set(core.getAllowedPropertyValues("Renderer", "PsfInterp")) == INTERP_VALUES
for interp in sorted(INTERP_VALUES):
    core.setProperty("Renderer", "PsfInterp", interp)
    generate_stack(core)
    assert snaps(1)[0].std() > 0, f"expected a non-blank frame at PsfInterp={interp}"
assert {"SaddlePoint", "ExtendedRange", "ExtendedRangeStrong"} <= set(core.getAllowedPropertyValues("Objective", "ZernikePreset"))
core.setProperty("Objective", "ZernikePreset", "ExtendedRangeStrong")
coeffs = core.getProperty("Objective", "ZernikeCoefficients").split()
assert len(coeffs) == 28 and float(coeffs[25]) == 0.3, f"expected 28 coefficients with j=25 = 0.3, got {coeffs}"
core.setProperty("Objective", "ZernikeCoefficients", "0 0 0 0 0 0.15 0 0 0 0 0 0 0 0 0")
zc = core.getProperty("Objective", "ZernikeCoefficients").split()
assert len(zc) == 28 and float(zc[5]) == 0.15, f"a 15-value Zernike list should be accepted, got {zc}"
assert not core.hasProperty("Objective", "PsfMaskType") and not core.hasProperty("Renderer", "PsfMaskType"), \
    "the DoubleHelix mask is not in Micro-Manager"
print(f"PSF OK: {len(INTERP_VALUES)} placement modes render, Zernike presets / 28 coefficients / 15-value lists")

# --- label modes, dye edits ----------------------------------------------------
MODES = {"dSTORM": [("CellField", "Microtubules_Dye", "AF647")],
         "PALM": [("CellField", "Microtubules_Dye", "mEos3.2")],
         "DNA-PAINT": [],
         "WideField": [("CellField", "Microtubules_Dye", "mEGFP")]}
GAUSS = [("Renderer", "PsfModel", "Gaussian")]
mode_sig = {}
for mode, extra in MODES.items():
    fr = stack_frames(GAUSS + extra + [("CellField", "Microtubules_Mode", mode)], n=20,
                      dye_edits={"InitialOnSec": 0} if mode == "dSTORM" else None)
    assert fr.std() > 0, f"expected a non-blank {mode} movie"
    mode_sig[mode] = fr.mean() - 100.0
assert mode_sig["WideField"] > 10 * mode_sig["dSTORM"], f"the mean field should be far brighter than blinks: {mode_sig}"
DSTORM = GAUSS + [("CellField", "Microtubules_Dye", "AF647"), ("CellField", "Microtubules_Mode", "dSTORM")]
lib = stack_frames(DSTORM, n=100, dye_edits={"InitialOnSec": 0})
dim = stack_frames(DSTORM, n=100, dye_edits={"InitialOnSec": 0, "Qy": 0.05})
assert dim.mean() - 100.0 < 0.5 * (lib.mean() - 100.0), "a lower quantum yield should give less light"
print("Label modes OK: " + ", ".join(f"{m} {v:.2f}" for m, v in mode_sig.items()) + " ADU above offset; custom dye edit applies")

# --- EMCCD: excess noise doubles the variance; bit-depth clip --------------------
NO_DYES = [("CellField", "Microtubules_LabelingPct", "0"), ("CellField", "Microtubules_ImagerNm", "0")]
bg_only = GAUSS + NO_DYES + [("SampleHolder", "BackgroundPhotonsPerSec", "2000"), ("Camera", "ReadNoiseElectrons", "0"),
                             ("Camera", "OffsetStdADU", "0"), ("Camera", "sCMOS_GainStdPctPerPixel", "0"),
                             ("Camera", "GainElectronsPerADU", "1"), ("Camera", "DarkCurrentElectronsPerSec", "0")]
sc = stack_frames(bg_only, n=10)
em = stack_frames(bg_only + [("Camera", "CameraType", "EMCCD"), ("Camera", "EMCCD_CicElectrons", "0")], n=10)
fano_sc = sc.var(axis=0).mean() / (sc.mean() - 100.0)
fano_em = em.var(axis=0).mean() / (em.mean() - 100.0)
assert 0.8 < fano_sc < 1.2 and 1.7 < fano_em < 2.3, f"variance/mean sCMOS {fano_sc:.2f}, EMCCD {fano_em:.2f}"
em8 = stack_frames(bg_only + [("Camera", "CameraType", "EMCCD"), ("Camera", "BitDepth", "8")], n=2)
assert em8.max() <= 255, f"EMCCD BitDepth=8 must clip at 255, got {em8.max()}"
print(f"EMCCD OK: variance/mean sCMOS {fano_sc:.2f}, EMCCD {fano_em:.2f}; 8-bit clip holds")

# --- illumination profile and background fade ------------------------------------
flat_bg = GAUSS + NO_DYES + [("SampleHolder", "BackgroundPhotonsPerSec", "400")]
illum = stack_frames(flat_bg + [("Lasers", "IlluminationProfile", "Gaussian")], n=5).mean(axis=0) - 100
assert illum[:10, :10].mean() < 0.5 * illum[54:74, 54:74].mean(), "Gaussian illumination must dim the corners"
decay = stack_frames(flat_bg + [("SampleHolder", "BackgroundDecaySec", "0.5")], n=40)
assert decay[-1].mean() < decay[0].mean(), "background fade must lower the background over time"
print("Background OK: illumination dims corners, fade decays")

# --- GPU vs CPU: the same frames up to float32 rounding (skipped without a usable GPU) ---
KERNEL = [("Objective", "PsfKernelHalfWidthNm", "3000")]
gpu_frames = stack_frames(KERNEL + [("Renderer", "UseGpu", "On")], n=10)
status = core.getProperty("Renderer", "GpuStatus")
if status.startswith("GPU"):
    cpu_frames = stack_frames(KERNEL + [("Renderer", "UseGpu", "Off")], n=10)
    same = (gpu_frames == cpu_frames).mean()
    assert same > 0.999, f"GPU/CPU frames agree on only {100*same:.4f}% of pixels"
    print(f"GPU OK ({status}): {100*same:.4f}% of pixels identical to the CPU path")
else:
    print(f"GPU check skipped: {status}")

core.unloadAllDevices()
print(f"All inSiliScope adapter checks passed ({time.time() - t_start:.0f} s).")
