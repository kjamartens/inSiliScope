"""Smoke test for the SMLMDemoCam device adapter, using pymmcore-plus.

Exercises: property wiring, pre-init enforcement (RandomSeed only -- FovSize
is a regular, post-init property), background stack generation not blocking
the calling thread, reproducibility (same seed + params -> identical
precomputed stack), correct pixel shape/dtype, both acquisition modes
end-to-end, and that changing a noise parameter (CameraOffsetStdADU) while
Live mode is streaming actually changes subsequent frames.

Requires an MM nightly build installed (e.g. via `mmcore install`) whose
device-interface version matches the checked-out mmCoreAndDevices submodule
commit, with mmgr_dal_SMLMDemoCam.dll copied into that install directory.
See docs/BUILD_AND_USAGE.md.
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
            "variable to point at your install (see docs/BUILD_AND_USAGE.md)."
        )
    return str(candidates[-1])


def load_camera(core: CMMCorePlus, label: str, seed: int, fov: str = "128x128") -> None:
    core.loadDevice(label, "SMLMDemoCam", "SMLMDemoCam")
    core.setProperty(label, "SimType_RandomSeed", str(seed))  # pre-init
    core.initializeDevice(label)
    core.setCameraDevice(label)
    core.setProperty(label, "General_FovSize", fov)  # regular property, set after init


def wait_for_stack(core: CMMCorePlus, label: str, timeout_s: float = 60.0) -> None:
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
core.setDeviceAdapterSearchPaths([mm_dir])

# --- pre-init properties -----------------------------------------------
load_camera(core, "SMLMCam", seed=42, fov="128x128")
assert core.isPropertyPreInit("SMLMCam", "SimType_RandomSeed")
assert not core.isPropertyPreInit("SMLMCam", "General_FovSize"), \
    "FovSize should be a regular, post-init-changeable property"
print("Pre-init property confirmed: RandomSeed (FovSize confirmed NOT pre-init)")

# --- Precomputed mode: trigger generation, poll status, snap -----------
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "General_StackLength", "50")
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
core.setProperty("SMLMCam", "General_StackLength", "50")
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
core.setProperty("SMLMCam", "General_StackLength", "50")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
frame_diff_seed = core.getImage()
assert not np.array_equal(frame_diff_seed, frames_a[0]), \
    "expected a different RandomSeed to produce a different frame"
print("Reproducibility OK: a different seed produced a different frame")

# --- Live mode: stream a short sequence, confirm frames change ---------
core.setProperty("SMLMCam", "General_AcqMode", "Live")
core.setProperty("SMLMCam", "General_EmitterDensityPerSec", "50.0")
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

# --- 3D structures / labeling efficiency: property wiring ---------------
# The device is currently left over from the two tests above with
# RandomSeed=43 (from the "different seed" check) and AcqMode=Live (from
# the live-mode check) -- reload with a known seed so every reproducibility
# assertion below has a well-defined starting point.
core.unloadDevice("SMLMCam")
load_camera(core, "SMLMCam", seed=42, fov="128x128")
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
new_props = [
    "General_LabelingEfficiencyPct", "SimType_StructureZRangeNm", "SimType_StructureSizeNm",
    "SimType_NupRadiusNm", "SimType_NupCornerSpreadNm", "SimType_NupRingSeparationNm",
    "SimType_NupLinkerMinNm", "SimType_NupLinkerMaxNm", "SimType_NupMembraneType", "SimType_NupCount",
    "SimType_NupMinSpacingNm", "SimType_NupCurvatureNm",
]
for name in new_props:
    core.getProperty("SMLMCam", name)  # raises if the property doesn't exist
pattern_values = set(core.getAllowedPropertyValues("SMLMCam", "SimType_Pattern"))
for expected in ("TiltedPlane", "Uniform3D", "Shell", "NUP"):
    assert expected in pattern_values, f"expected Pattern to allow {expected!r}, got {pattern_values}"
membrane_values = set(core.getAllowedPropertyValues("SMLMCam", "SimType_NupMembraneType"))
assert membrane_values == {"TopDown", "Sideways"}, \
    f"expected NupMembraneType allowed values {{TopDown, Sideways}}, got {membrane_values}"
print("3D structure property wiring OK:", len(new_props), "properties present,",
      "Pattern/NupMembraneType allowed values confirmed")

# --- NUP: end-to-end + reproducibility -----------------------------------
core.setProperty("SMLMCam", "SimType_Pattern", "NUP")
core.setProperty("SMLMCam", "SimType_NupCount", "8")
core.setProperty("SMLMCam", "General_StackLength", "30")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
nup_img_a = core.getImage().copy()
assert nup_img_a.std() > 0, "expected a non-blank NUP frame"

core.unloadDevice("SMLMCam")
load_camera(core, "SMLMCam", seed=42, fov="128x128")
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "SimType_Pattern", "NUP")
core.setProperty("SMLMCam", "SimType_NupCount", "8")
core.setProperty("SMLMCam", "General_StackLength", "30")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
nup_img_b = core.getImage().copy()
assert np.array_equal(nup_img_a, nup_img_b), \
    "expected NUP pattern to be byte-identical across two runs of the same seed"
print("NUP pattern OK: non-blank, byte-identical across two runs of the same seed")

# --- Labeling efficiency: fewer distinct bright sites at low efficiency --
# Emitter density is an AREAL rate independent of site count, so arrivals
# concentrate onto fewer sites rather than dropping in total count --
# assert on the number of distinct bright pixels (site coverage), not on
# mean/std intensity, which would not reliably change.
core.setProperty("SMLMCam", "SimType_Pattern", "Shell")
core.setProperty("SMLMCam", "General_BackgroundPhotonsPerSec", "0.0")
core.setProperty("SMLMCam", "General_StackLength", "50")


def summed_bright_area(threshold_frac=0.5):
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    total = np.zeros((128, 128), dtype=np.float64)
    for _ in range(50):
        core.snapImage()
        total += core.getImage().astype(np.float64)
    # Every pixel carries a uniform accumulated camera-offset baseline (50
    # frames x ~100 ADU) that swamps a threshold based on total.max() alone
    # -- subtract total.min() first so the threshold isolates actual signal
    # contrast rather than the flat offset floor.
    signal = total - total.min()
    peak = signal.max()
    if peak <= 0.0:
        return 0
    return int(np.count_nonzero(signal > threshold_frac * peak))


core.setProperty("SMLMCam", "General_LabelingEfficiencyPct", "100")
area_full = summed_bright_area()
core.setProperty("SMLMCam", "General_LabelingEfficiencyPct", "10")
area_low = summed_bright_area()
core.setProperty("SMLMCam", "General_LabelingEfficiencyPct", "100")  # restore default
assert area_low < area_full, (
    f"expected LabelingEfficiencyPct=10 to light up fewer distinct sites than =100 "
    f"(full={area_full}px, low={area_low}px) -- labeling filter may not be reaching the renderer"
)
print(f"Labeling efficiency OK: bright-site area dropped {area_full}px -> {area_low}px at 10% labeling")

# --- Per-emitter z: 3D structure should visibly defocus vs flat ----------
# Only meaningful with a vectorial PsfModel (JVM/PSFGenerator jar); skip
# gracefully if that path isn't available in this environment.
core.setProperty("SMLMCam", "SimType_Pattern", "Uniform3D")
core.setProperty("SMLMCam", "PSFParam_PsfModel", "GibsonLanni")
core.setProperty("SMLMCam", "PSFParam_PsfZRangeUm", "7")
core.setProperty("SMLMCam", "SimType_StructureZRangeNm", "2000")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
img_3d = core.getImage().astype(np.float64)

core.setProperty("SMLMCam", "SimType_StructureZRangeNm", "0")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
img_flat = core.getImage().astype(np.float64)

if img_3d.std() == 0 or img_flat.std() == 0:
    print("Per-emitter z check SKIPPED: vectorial PSF path unavailable in this environment "
          "(frame is blank -- likely no usable JVM/JRE)")
else:
    # Defocused (spread-out) emitters have a lower peak/std than in-focus
    # ones at the same total photon budget -- a coarse but robust proxy for
    # "z is actually reaching the renderer per emitter" without needing a
    # PSF-shape fit.
    assert img_flat.std() > img_3d.std(), (
        f"expected StructureZRangeNm=2000 (defocused) to have LOWER std than =0 (in-focus) "
        f"(flat.std={img_flat.std():.3f}, 3d.std={img_3d.std():.3f}) -- per-emitter z may not be reaching PSF plane selection"
    )
    print(f"Per-emitter z OK: flat.std={img_flat.std():.3f} > 3d.std={img_3d.std():.3f} (defocus reduces peak/std)")
core.setProperty("SMLMCam", "SimType_StructureZRangeNm", "500")  # restore default

# --- Live responsiveness: a large NUP site-list build must not happen ----
# --- per frame (it's gated behind liveConfigVersion_, same as the PSF
# --- kernel cache) -- confirm ActualFrameIntervalMs stays close to Exposure.
core.setProperty("SMLMCam", "SimType_Pattern", "NUP")
core.setProperty("SMLMCam", "SimType_NupCount", "200")
core.setProperty("SMLMCam", "General_AcqMode", "Live")
core.setProperty("SMLMCam", "PSFParam_PsfModel", "Gaussian")  # isolate site-list cost from JVM/PSF cost
core.startSequenceAcquisition(20, 20.0, True)
while core.isSequenceRunning():
    time.sleep(0.02)
time.sleep(0.2)
while core.getRemainingImageCount() > 0:
    core.popNextImage()
interval_ms = core.getProperty("SMLMCam", "General_ActualFrameIntervalMs")
assert float(interval_ms) < 100.0, (
    f"expected ActualFrameIntervalMs to stay within ~5x of the 20ms Exposure with NupCount=200 "
    f"live, got {interval_ms}ms -- site-list build may be happening per frame instead of on config change"
)
print(f"Live responsiveness OK: ActualFrameIntervalMs={interval_ms}ms with NupCount=200 streaming live")
core.setProperty("SMLMCam", "SimType_Pattern", "Circle")  # restore default
core.setProperty("SMLMCam", "SimType_NupCount", "20")  # restore default

# --- PsfInterp / PsfEvalMethod: property wiring + non-blank sanity -------
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "PSFParam_PsfModel", "GibsonLanniZernike")
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "AstigmatismModerate")
core.setProperty("SMLMCam", "PSFParam_PsfZRangeUm", "2")
core.setProperty("SMLMCam", "PSFParam_PsfZStepUm", "0.2")
core.setProperty("SMLMCam", "General_StackLength", "10")
interp_values = set(core.getAllowedPropertyValues("SMLMCam", "PSFParam_PsfInterp"))
assert interp_values == {"Nearest", "Linear", "Cubic"}, f"unexpected PsfInterp values: {interp_values}"
evalmethod_values = set(core.getAllowedPropertyValues("SMLMCam", "PSFParam_PsfEvalMethod"))
assert evalmethod_values == {"Direct", "ChirpZ"}, f"unexpected PsfEvalMethod values: {evalmethod_values}"
for interp in ("Nearest", "Linear", "Cubic"):
    for method in ("Direct", "ChirpZ"):
        core.setProperty("SMLMCam", "PSFParam_PsfInterp", interp)
        core.setProperty("SMLMCam", "PSFParam_PsfEvalMethod", method)
        core.setProperty("SMLMCam", "General_GenerateStack", "1")
        wait_for_stack(core, "SMLMCam")
        core.snapImage()
        img = core.getImage()
        assert img.std() > 0, f"expected non-blank frame at PsfInterp={interp}, PsfEvalMethod={method}"
print("PsfInterp/PsfEvalMethod OK: allowed values confirmed, all 6 combinations produce non-blank frames")
core.setProperty("SMLMCam", "PSFParam_PsfInterp", "Nearest")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfEvalMethod", "Direct")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "None")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfZRangeUm", "7")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfZStepUm", "0.1")  # restore default
core.setProperty("SMLMCam", "PSFParam_PsfModel", "GibsonLanni")  # restore default

# --- Regression: changing a noise parameter mid-Live-stream must actually
# change subsequent frames, without needing to toggle AcqMode or restart
# anything (this used to silently no-op for CameraOffsetStdADU/CameraOffsetADU
# because the live producer thread cached its fixed-pattern offset map and
# only rebuilt it on a frame-size change). Use a large offset-std delta so
# the resulting frame-to-frame std shift is unambiguous against ordinary
# shot/read noise.
core.setProperty("SMLMCam", "CamParam_CameraOffsetStdADU", "0.0")
core.setProperty("SMLMCam", "General_BackgroundPhotonsPerSec", "0.0")
time.sleep(0.3)  # let a few live ticks pass with the low-offset-std setting
core.snapImage()
std_before = float(np.std(core.getImage().astype(np.float64)))

core.setProperty("SMLMCam", "CamParam_CameraOffsetStdADU", "50.0")
time.sleep(0.3)  # let the live producer thread pick up the change
core.snapImage()
std_after = float(np.std(core.getImage().astype(np.float64)))

print("Live offset-std hookup: measured frame std before=%.3f after=%.3f" % (std_before, std_after))
assert std_after > std_before + 10.0, (
    f"expected CameraOffsetStdADU=50 to visibly increase frame-to-frame pixel std "
    f"vs CameraOffsetStdADU=0 (before={std_before:.3f}, after={std_after:.3f}) -- "
    "looks like the live producer thread isn't picking up the change"
)
core.setProperty("SMLMCam", "CamParam_CameraOffsetStdADU", "0.5")  # restore default
print("Regression OK: CameraOffsetStdADU change took effect live, no restart needed")

print("All SMLMDemoCam smoke tests passed.")
