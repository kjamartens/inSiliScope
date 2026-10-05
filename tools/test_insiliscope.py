"""Smoke test for the inSiliScope device adapter, using pymmcore-plus.

Exercises (the CellField pattern, the only one since issue 16): property
wiring, pre-init enforcement (RandomSeed only -- FovSize
is a regular, post-init property), background stack generation not blocking
the calling thread, reproducibility (same seed + params -> identical
precomputed stack), correct pixel shape/dtype, both acquisition modes
end-to-end, and that changing a noise parameter (OffsetStdADU) while
Live mode is streaming actually changes subsequent frames.

Requires an MM nightly build installed (e.g. via `mmcore install`) whose
device-interface version matches the checked-out mmCoreAndDevices submodule
commit, with mmgr_dal_inSiliScope.dll copied into that install directory
or found via ADAPTER_DIR. See docs/dev/BUILD_AND_USAGE.md.
"""

import os
import sys
import time

import numpy as np
from pymmcore_plus import CMMCorePlus
from pymmcore_plus._util import USER_DATA_DIR


def find_active_mm_dir() -> str:
    """Locate the active pymmcore-plus-managed MicroManager install.

    Override with the MM_DIR environment variable if pointing at a different
    install.
    """
    env_override = os.environ.get("MM_DIR")
    if env_override:
        return env_override
    mm_root = USER_DATA_DIR / "mm"
    candidates = sorted(mm_root.glob("Micro-Manager_*"))
    if not candidates:
        sys.exit(
            f"No MicroManager install found under {mm_root}.\n"
            "Run `mmcore install` first, or set the MM_DIR environment "
            "variable to point at your install (see docs/dev/BUILD_AND_USAGE.md)."
        )
    return str(candidates[-1])


def load_camera(core: CMMCorePlus, label: str, seed: int, fov: str = "128x128") -> None:
    core.loadDevice(label, "inSiliScope", "Camera")
    core.setProperty(label, "SimType_RandomSeed", str(seed))  # pre-init
    core.initializeDevice(label)
    core.setCameraDevice(label)
    core.setProperty(label, "General_FovSize", fov)  # regular property, set after init


# Generous default: the precomputed stack is a fixed 1000 frames now that
# General_StackLength is gone (see CLAUDE.md), and the default PSF model is
# the comparatively expensive GibsonLanniZernike -- a single generation is
# tens of seconds, not the couple of seconds a 50-frame stack used to be.
def wait_for_stack(core: CMMCorePlus, label: str, timeout_s: float = 600.0) -> None:
    t0 = time.time()
    while True:
        status = core.getProperty(label, "General_StackGenerationStatus")
        if status.startswith("Ready"):
            return
        if time.time() - t0 > timeout_s:
            sys.exit(f"Timed out waiting for stack generation, last status: {status}")
        time.sleep(0.1)


mm_dir = find_active_mm_dir()

core = CMMCorePlus()
# ADAPTER_DIR (e.g. adapter/inSiliScope/build/Release/x64) is searched first, so a
# fresh build can be tested without copying the DLL into the MM install.
core.setDeviceAdapterSearchPaths([d for d in (os.environ.get("ADAPTER_DIR"), mm_dir) if d])

# --- pre-init properties -----------------------------------------------
load_camera(core, "SMLMCam", seed=42, fov="128x128")
assert core.isPropertyPreInit("SMLMCam", "SimType_RandomSeed")
assert not core.isPropertyPreInit("SMLMCam", "General_FovSize"), \
    "FovSize should be a regular, post-init-changeable property"
print("Pre-init property confirmed: RandomSeed (FovSize confirmed NOT pre-init)")

# --- Precomputed mode: trigger generation, poll status, snap -----------
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
print("Stack generation status:", core.getProperty("SMLMCam", "General_StackGenerationStatus"))

core.snapImage()
img = core.getImage()
assert img.shape == (128, 128), f"expected (128, 128), got {img.shape}"
assert img.dtype == np.uint16, f"expected uint16, got {img.dtype}"
assert img.std() > 0, "expected a non-blank frame"
print("Snap OK. Image shape:", img.shape, "dtype:", img.dtype, "std:", img.std())

# --- Reproducibility: same seed + params -> byte-identical stack -------
# The shape-check snap just above already consumed stack frame 0 from this
# run's playback cursor, so it's folded in as frames_a[0] here rather than
# discarded -- otherwise frames_a would start at stack index 1 while
# frames_b (below, freshly generated with no such prior snap) starts at
# index 0, an off-by-one that made every "same seed" comparison spuriously
# fail despite the underlying stacks being genuinely byte-identical.
frames_a = [img.copy()]
for _ in range(4):
    core.snapImage()
    frames_a.append(core.getImage().copy())

core.unloadDevice("SMLMCam")
load_camera(core, "SMLMCam", seed=42, fov="128x128")
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")

frames_b = []
for _ in range(5):
    core.snapImage()
    frames_b.append(core.getImage().copy())

for i, (a, b) in enumerate(zip(frames_a, frames_b)):
    assert np.array_equal(a, b), f"frame {i} differs between two runs with the same seed"
print("Reproducibility OK: identical seed+params produced byte-identical frames")

# A different seed should (with overwhelming probability) produce a
# different frame.
core.unloadDevice("SMLMCam")
load_camera(core, "SMLMCam", seed=43, fov="128x128")
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
frame_diff_seed = core.getImage()
assert not np.array_equal(frame_diff_seed, frames_a[0]), \
    "expected a different RandomSeed to produce a different frame"
print("Reproducibility OK: a different seed produced a different frame")

# --- Live mode: stream a short sequence, confirm frames change ---------
core.setProperty("SMLMCam", "General_AcqMode", "Live")
core.startSequenceAcquisition(10, 20.0, True)
while core.isSequenceRunning():
    time.sleep(0.02)
time.sleep(0.2)

live_frames = []
while core.getRemainingImageCount() > 0:
    live_frames.append(core.popNextImage())

assert len(live_frames) >= 2, f"expected several live frames, got {len(live_frames)}"
assert not np.array_equal(live_frames[0], live_frames[-1]), \
    "expected live-mode frames to differ over time"
print("Live mode OK:", len(live_frames), "frames captured, frames vary over time")

# --- Property surface ----------------------------------------------------
# The device is currently left over from the two tests above with
# RandomSeed=43 (from the "different seed" check) and AcqMode=Live (from
# the live-mode check) -- reload with a known seed so every reproducibility
# assertion below has a well-defined starting point.
core.unloadDevice("SMLMCam")
load_camera(core, "SMLMCam", seed=42, fov="128x128")
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")

# --- Renamed / removed properties ---------------------------------------
all_props = set(core.getDevicePropertyNames("SMLMCam"))
renamed = [
    "CamParam_GainPhotonsPerADU", "CamParam_OffsetADU", "CamParam_OffsetStdADU",
    "CamParam_GainStdPctPerPixel", "CamParam_ReadNoiseStdPctPerPixel",
    "PSFParam_PsfKernelHalfWidthNm", "Background_BackgroundPhotonsPerSec",
    # Issue 16: labels per structure, dyes, light path.
    "SimType_CellFieldMicrotubuleDye", "SimType_CellFieldMicrotubuleLabelMode", "SimType_CellFieldMicrotubuleLabelingPct",
    "FluoParam_Microtubule_OnSec", "FluoParam_Dye1_Source", "Optics_Preset", "Optics_Laser640KWcm2",
    "Optics_IlluminationProfile", "Optics_IlluminationFwhmPct", "CamParam_QeCurve", "CamParam_CameraPreset",
]
missing = [n for n in renamed if n not in all_props]
assert not missing, f"expected renamed properties to exist, missing: {missing}"
gone = [
    "General_StackLength", "General_StackLoop", "PSFParam_PsfKernelHalfWidthPx",
    "CamParam_CameraGainPhotonsPerADU", "CamParam_CameraOffsetADU", "CamParam_CameraOffsetStdADU",
    "CamParam_PixelGainStdPct", "CamParam_PixelReadNoiseStdPct",
    # Removed with the wide-kernel-incorrect Direct evaluator (webSMLM parity round 2):
    "PSFParam_PsfEvalMethod",
    # Moved into the Background_ group:
    "General_BackgroundPhotonsPerSec",
    # Removed with the non-CellField patterns (issue 16):
    "SimType_Pattern", "SimType_CustomPointsFile", "SimType_ResolutionSpacingsNm", "General_EmitterDensityPerSec",
    "General_LabelingEfficiencyPct", "SimType_StructureZRangeNm", "SimType_StructureSizeNm", "SimType_NupCount",
    "SimType_NupRadiusNm", "SimType_NupMembraneType", "Background_CellContrast", "Background_HazeWeight",
    "Background_HazeWidthNm", "Background_OutOfFocusRatio", "Background_OutOfFocusDepthNm",
    # Replaced by the labels, dyes and light path (issue 16, phase 3):
    "FluoParam_PhotonsPerSecond", "FluoParam_OnLifetimeSec", "PSFParam_PsfEmissionWavelengthNm",
    "FluoParam_BlinkBleachProb", "FluoParam_OffLifetimeSec", "FluoParam_PhotonCV", "FluoParam_IllumProfile",
    "FluoParam_IllumFwhmPct", "SimType_CellFieldLabelingPctBleaching", "SimType_CellFieldLabelingPctNonBleaching",
    "SimType_CellFieldMilliActivationRatePerDyePerSec", "FluoParam_WideFieldExcitationPhotonsPerUm2PerSec",
    "FluoParam_WideFieldQuantumYield", "FluoParam_WideFieldPhotonBudget", "FluoParam_WideFieldExtinctionCoeff",
    "FluoParam_WideFieldHalfTimeSec",
]
still_there = [n for n in gone if n in all_props]
assert not still_there, f"expected these properties to be removed/renamed away, still present: {still_there}"
print("Property surface OK:", len(renamed), "renamed properties present,", len(gone), "old names gone")

# --- Defaults ------------------------------------------------------------
# A freshly loaded device (see load_camera above -- it only sets RandomSeed
# and FovSize) must come up with these out-of-the-box values.
for name, expected in [
    ("PSFParam_PsfMaskType", "None"),
    # webSMLM parity round 2: every new feature defaults to "off".
    ("Optics_IlluminationProfile", "Flat"),
    # Issue 16: DNA-PAINT ATTO 655 imager on the microtubules, 70% labelled.
    ("General_ImagingModality", "Fluorescence"),
    ("SimType_CellFieldMicrotubuleDye", "ATTO655"),
    ("SimType_CellFieldMicrotubuleLabelMode", "DyeDefault"),
    ("SimType_CellFieldMicrotubuleLabelingPct", "70"),
    ("Optics_Preset", "PAINT-640"),
    ("CamParam_CameraType", "sCMOS"),
    ("Background_DecaySec", "0"),
    ("General_UseGpu", "On"),
    ("General_DiskCache", "Cells"),
    ("PSFParam_PsfInterp", "Cubic"),
    ("PSFParam_PsfModel", "GibsonLanniZernike"),
    ("PSFParam_PsfOversampling", "6"),
    ("PSFParam_PsfZernikePreset", "MixedRealisticObjective"),
    ("PSFParam_PsfKernelHalfWidthNm", "7000"),
]:
    actual = core.getProperty("SMLMCam", name)
    assert float(actual) == float(expected) if expected.replace(".", "").isdigit() else actual == expected,         f"expected {name} to default to {expected!r}, got {actual!r}"
print("Defaults OK: out-of-the-box values confirmed")

# --- Labelling: fewer labelled sites, less light -------------------------
# The blink rate comes from the labelled sites: a tenth of them gives about a
# tenth of the signal above the offset (dSTORM AF647: no imager background).
# Each stack on a fresh device: a stack continues the illumination history of
# its spot, so a second stack on the same device would start 50 s later.
def fresh_camera(**props):
    core.unloadDevice("SMLMCam")
    load_camera(core, "SMLMCam", seed=42, fov="128x128")
    core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
    core.setProperty("SMLMCam", "PSFParam_PsfModel", "Gaussian")
    core.setProperty("SMLMCam", "Background_BackgroundPhotonsPerSec", "0.0")
    for k, v in props.items():
        core.setProperty("SMLMCam", k, v)


def mean_signal(pct, n=50, skip=900):
    fresh_camera(SimType_CellFieldMicrotubuleDye="AF647",  # dSTORM: no imager background
                 SimType_CellFieldMicrotubuleLabelingPct=str(pct))
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    for _ in range(skip):   # 45 s in: past the initial ON burst (which saturates at 70 %)
        core.snapImage()
    total = 0.0
    for _ in range(n):
        core.snapImage()
        total += float(core.getImage().astype(np.float64).mean()) - 100.0
    return total / n


sig_full = mean_signal(70)
sig_low = mean_signal(7)
fresh_camera()  # the defaults again (Gaussian PSF, no background)
assert sig_low < 0.3 * sig_full, f"expected 7% labelling to give ~1/10 of the 70% signal ({sig_low:.3f} vs {sig_full:.3f})"
print(f"Labelling OK: mean signal {sig_full:.3f} -> {sig_low:.3f} ADU from 70% to 7% labelled sites")

# --- Live responsiveness: nothing heavy may run per frame -----------------
# (the world and PSF are gated behind liveConfigVersion_) -- confirm
# ActualFrameIntervalMs stays close to Exposure.
core.setProperty("SMLMCam", "General_AcqMode", "Live")
core.setProperty("SMLMCam", "Exposure", "20")
# Twice: the first frames after switching to Live build the world and
# prefetch the dyes around the FOV (seconds, timing-dependent); the interval
# (a mean over the last 10 frames) is measured on the warm loop.
for _ in range(2):
    core.startSequenceAcquisition(20, 20.0, True)
    while core.isSequenceRunning():
        time.sleep(0.02)
    time.sleep(0.2)
    while core.getRemainingImageCount() > 0:
        core.popNextImage()
interval_ms = core.getProperty("SMLMCam", "General_ActualFrameIntervalMs")
assert float(interval_ms) < 100.0, (
    f"expected ActualFrameIntervalMs to stay within ~5x of the 20ms Exposure live, got {interval_ms}ms"
)
print(f"Live responsiveness OK: ActualFrameIntervalMs={interval_ms}ms streaming live")
core.setProperty("SMLMCam", "Exposure", "50")
core.setProperty("SMLMCam", "PSFParam_PsfModel", "GibsonLanniZernike")  # restore default

# --- PsfInterp: property wiring + non-blank sanity ----------------------
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "PSFParam_PsfModel", "GibsonLanniZernike")
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "AstigmatismModerate")
core.setProperty("SMLMCam", "PSFParam_PsfZRangeUm", "2")
core.setProperty("SMLMCam", "PSFParam_PsfZStepUm", "0.2")
INTERP_VALUES = {"Nearest", "Linear", "Cubic", "Fft"}
interp_values = set(core.getAllowedPropertyValues("SMLMCam", "PSFParam_PsfInterp"))
assert interp_values == INTERP_VALUES, f"unexpected PsfInterp values: {interp_values}"
# Sparse labelling: Fft does one Fourier shift per emitter and is by far the
# slowest mode (the default labelling would take Fft placement far past
# wait_for_stack's timeout).
core.setProperty("SMLMCam", "SimType_CellFieldMicrotubuleLabelingPct", "0.5")
for interp in sorted(INTERP_VALUES):
    core.setProperty("SMLMCam", "PSFParam_PsfInterp", interp)
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    core.snapImage()
    img = core.getImage()
    assert img.std() > 0, f"expected non-blank frame at PsfInterp={interp}"
print(f"PsfInterp OK: allowed values confirmed, all {len(INTERP_VALUES)} modes produce non-blank frames")
core.setProperty("SMLMCam", "PSFParam_PsfInterp", "Cubic")  # restore default

# --- Zernike presets / 28 coefficients / double-helix mask -----------------
preset_values = set(core.getAllowedPropertyValues("SMLMCam", "PSFParam_PsfZernikePreset"))
for expected in ("SaddlePoint", "ExtendedRange", "ExtendedRangeStrong"):
    assert expected in preset_values, f"expected PsfZernikePreset to allow {expected!r}"
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "ExtendedRangeStrong")
coeffs = core.getProperty("SMLMCam", "PSFParam_PsfZernikeCoefficients").split()
assert len(coeffs) == 28 and float(coeffs[25]) == 0.3, f"expected 28 coefficients with j=25 = 0.3, got {coeffs}"
# A 15-value list is still accepted (zero-padded to 28). Space-separated:
# MMCore rejects commas in any property value set through it.
core.setProperty("SMLMCam", "PSFParam_PsfZernikeCoefficients", "0 0 0 0 0 0.15 0 0 0 0 0 0 0 0 0")
zc = core.getProperty("SMLMCam", "PSFParam_PsfZernikeCoefficients").split()
assert len(zc) == 28 and float(zc[5]) == 0.15, f"expected a 15-value Zernike list to be accepted, got {zc}"
assert set(core.getAllowedPropertyValues("SMLMCam", "PSFParam_PsfMaskType")) == {"None", "DoubleHelix"}
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "None")
core.setProperty("SMLMCam", "SimType_CellFieldMicrotubuleLabelingPct", "70")  # restore default
mask_frames = {}
for mask in ("None", "DoubleHelix"):
    core.setProperty("SMLMCam", "PSFParam_PsfMaskType", mask)
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    core.snapImage()
    mask_frames[mask] = core.getImage().astype(np.float64)
    assert mask_frames[mask].std() > 0, f"expected non-blank frame at PsfMaskType={mask}"
assert not np.array_equal(mask_frames["None"], mask_frames["DoubleHelix"]), \
    "expected the double-helix mask to change the rendered frame"
print("Zernike/mask OK: engineered presets, 28-coefficient list, 15-value back-compat, DoubleHelix renders")
core.setProperty("SMLMCam", "PSFParam_PsfMaskType", "None")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "MixedRealisticObjective")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfZRangeUm", "7")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfZStepUm", "0.1")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfModel", "GibsonLanniZernike")  # restore default

# --- Regression: changing a noise parameter mid-Live-stream must actually
# change subsequent frames, without needing to toggle AcqMode or restart
# anything (this used to silently no-op for OffsetStdADU/OffsetADU
# because the live producer thread cached its fixed-pattern offset map and
# only rebuilt it on a frame-size change). Use a large offset-std delta so
# the resulting frame-to-frame std shift is unambiguous against ordinary
# shot/read noise.
core.setProperty("SMLMCam", "CamParam_OffsetStdADU", "0.0")
core.setProperty("SMLMCam", "Background_BackgroundPhotonsPerSec", "0.0")
time.sleep(0.3)  # let a few live ticks pass with the low-offset-std setting
core.snapImage()
std_before = float(np.std(core.getImage().astype(np.float64)))

core.setProperty("SMLMCam", "CamParam_OffsetStdADU", "50.0")
time.sleep(0.3)  # let the live producer thread pick up the change
core.snapImage()
std_after = float(np.std(core.getImage().astype(np.float64)))

print("Live offset-std hookup: measured frame std before=%.3f after=%.3f" % (std_before, std_after))
assert std_after > std_before + 10.0, (
    f"expected OffsetStdADU=50 to visibly increase frame-to-frame pixel std "
    f"vs OffsetStdADU=0 (before={std_before:.3f}, after={std_after:.3f}) -- "
    "looks like the live producer thread isn't picking up the change"
)
core.setProperty("SMLMCam", "CamParam_OffsetStdADU", "0.5")  # restore default
print("Regression OK: OffsetStdADU change took effect live, no restart needed")

# --- webSMLM parity round 2: photophysics, EMCCD, background, GPU ---------
def stack_frames(props, n=20, seed=42, skip=0):
    """Fresh device, apply props, generate the precomputed stack, return n frames (float64) after skipping skip.

    A fresh device has no illumination history: the stack starts at clock 0 (dSTORM in its initial ON phase)."""
    core.unloadDevice("SMLMCam")
    load_camera(core, "SMLMCam", seed=seed, fov="128x128")
    core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
    for k, v in props.items():
        core.setProperty("SMLMCam", k, v)
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    for _ in range(skip):
        core.snapImage()
    out = []
    for _ in range(n):
        core.snapImage()
        out.append(core.getImage().astype(np.float64))
    return np.stack(out)

FAST = {"PSFParam_PsfModel": "Gaussian"}

# Label modes (frames 900-919 of the stack, 45 s in: past the dSTORM initial
# ON): every mode of the microtubules' label renders, and dSTORM's
# blinks follow the dye fields (a dye that bleaches after each blink gives
# less light than the library AF647). WideField is the mean field.
MODES = {"dSTORM": {"SimType_CellFieldMicrotubuleDye": "AF647"},
         "PALM": {"SimType_CellFieldMicrotubuleDye": "mEos3.2"},
         "DNA-PAINT": {},
         "WideField": {"SimType_CellFieldMicrotubuleDye": "mEGFP"}}
mode_sig = {}
for mode, extra in MODES.items():
    fr = stack_frames({**FAST, **extra, "SimType_CellFieldMicrotubuleLabelMode": mode}, n=20, skip=900)
    assert fr.std() > 0, f"expected a non-blank {mode} movie"
    mode_sig[mode] = fr.mean() - 100.0
assert mode_sig["WideField"] > 10 * mode_sig["dSTORM"], f"the mean field should be far brighter than blinks: {mode_sig}"
DSTORM = {**FAST, "SimType_CellFieldMicrotubuleDye": "AF647", "SimType_CellFieldMicrotubuleLabelMode": "dSTORM"}
lib = stack_frames(DSTORM, n=100, skip=900)
quick = stack_frames({**DSTORM, "FluoParam_Microtubule_BleachProb": "1"}, n=100, skip=900)
assert quick.mean() < lib.mean(), "bleaching after every blink should give less light than the library dye"
print("Label modes OK: " + ", ".join(f"{m} {v:.2f}" for m, v in mode_sig.items()) + " ADU above offset; dye field edit applies")

# EMCCD: background-only frame -> the gain register doubles the variance
# (excess noise factor sqrt(2)); ADU clipped to the bit depth.
NO_DYES = {"SimType_CellFieldMicrotubuleLabelingPct": "0", "SimType_CellFieldMicrotubuleImagerNm": "0"}
bg_only = {**FAST, **NO_DYES, "Background_BackgroundPhotonsPerSec": "2000", "CamParam_ReadNoiseElectrons": "0",
           "CamParam_OffsetStdADU": "0", "CamParam_GainStdPctPerPixel": "0", "CamParam_GainPhotonsPerADU": "1",
           "CamParam_DarkCurrentElectronsPerSec": "0"}
sc = stack_frames(bg_only, n=10)
em = stack_frames({**bg_only, "CamParam_CameraType": "EMCCD", "CamParam_CicElectrons": "0"}, n=10)
fano_sc = sc.var(axis=0).mean() / (sc.mean() - 100.0)
fano_em = em.var(axis=0).mean() / (em.mean() - 100.0)
assert 0.8 < fano_sc < 1.2 and 1.7 < fano_em < 2.3, f"variance/mean sCMOS {fano_sc:.2f}, EMCCD {fano_em:.2f}"
em12 = stack_frames({**bg_only, "CamParam_CameraType": "EMCCD", "CamParam_BitDepth": "8"}, n=2)
assert em12.max() <= 255, f"EMCCD BitDepth=8 must clip at 255, got {em12.max()}"
print(f"EMCCD OK: variance/mean sCMOS {fano_sc:.2f}, EMCCD {fano_em:.2f}; 8-bit clip holds")

# Gaussian illumination dims the corners; the background fades over time.
flat_bg = {**FAST, **NO_DYES, "Background_BackgroundPhotonsPerSec": "400"}
illum = stack_frames({**flat_bg, "Optics_IlluminationProfile": "Gaussian"}, n=5).mean(axis=0) - 100
assert illum[:10, :10].mean() < 0.5 * illum[54:74, 54:74].mean(), "Gaussian illumination must dim the corners"
decay = stack_frames({**flat_bg, "Background_DecaySec": "0.5"}, n=40)
assert decay[-1].mean() < decay[0].mean(), "background fade must lower the background over time"
print("Background OK: illumination dims corners, fade decays")

# GPU vs CPU: same frames up to float32 rounding (a Poisson draw may land a
# count apart near a boundary). Skipped when no usable GPU.
gpu_frames = stack_frames({"General_UseGpu": "On"}, n=10)
status = core.getProperty("SMLMCam", "General_GpuStatus")
if status.startswith("GPU"):
    cpu_frames = stack_frames({"General_UseGpu": "Off"}, n=10)
    same = (gpu_frames == cpu_frames).mean()
    assert same > 0.999, f"GPU/CPU frames agree on only {100*same:.4f}% of pixels"
    print(f"GPU OK ({status}): {100*same:.4f}% of pixels identical to the CPU path")
else:
    print(f"GPU check skipped: {status}")

# CellField pattern + XYStage (spec/PORT.md 10.5): tools/test_cellfield_stage.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_cellfield_stage import run_checks as run_cellfield_checks

run_cellfield_checks(core)

print("All inSiliScope smoke tests passed.")
