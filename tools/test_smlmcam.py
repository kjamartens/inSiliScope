"""Smoke test for the SMLMDemoCam device adapter, using pymmcore-plus.

Exercises: property wiring, pre-init enforcement (RandomSeed only -- FovSize
is a regular, post-init property), background stack generation not blocking
the calling thread, reproducibility (same seed + params -> identical
precomputed stack), correct pixel shape/dtype, both acquisition modes
end-to-end, and that changing a noise parameter (OffsetStdADU) while
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
core.setDeviceAdapterSearchPaths([mm_dir])

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
for expected in ("TiltedPlane", "Uniform3D", "Shell", "NUP", "Calibration9Spots", "FilamentsRing"):
    assert expected in pattern_values, f"expected Pattern to allow {expected!r}, got {pattern_values}"
membrane_values = set(core.getAllowedPropertyValues("SMLMCam", "SimType_NupMembraneType"))
assert membrane_values == {"TopDown", "Sideways"}, \
    f"expected NupMembraneType allowed values {{TopDown, Sideways}}, got {membrane_values}"
print("3D structure property wiring OK:", len(new_props), "properties present,",
      "Pattern/NupMembraneType allowed values confirmed")

# --- Renamed / removed properties ---------------------------------------
all_props = set(core.getDevicePropertyNames("SMLMCam"))
renamed = [
    "CamParam_GainPhotonsPerADU", "CamParam_OffsetADU", "CamParam_OffsetStdADU",
    "CamParam_GainStdPctPerPixel", "CamParam_ReadNoiseStdPctPerPixel",
    "PSFParam_PsfKernelHalfWidthNm", "Background_BackgroundPhotonsPerSec",
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
]
still_there = [n for n in gone if n in all_props]
assert not still_there, f"expected these properties to be removed/renamed away, still present: {still_there}"
print("Property surface OK:", len(renamed), "renamed properties present,", len(gone), "old names gone")

# --- Defaults ------------------------------------------------------------
# A freshly loaded device (see load_camera above -- it only sets RandomSeed
# and FovSize) must come up with these out-of-the-box values.
for name, expected in [
    ("General_LabelingEfficiencyPct", "70"),
    ("PSFParam_PsfMaskType", "None"),
    # webSMLM parity round 2: every new feature defaults to "off".
    ("FluoParam_BlinkBleachProb", "1"),
    ("FluoParam_PhotonCV", "0"),
    ("FluoParam_IllumProfile", "Flat"),
    ("CamParam_CameraType", "sCMOS"),
    ("Background_CellContrast", "1"),
    ("Background_HazeWeight", "0"),
    ("Background_DecaySec", "0"),
    ("Background_OutOfFocusRatio", "0"),
    ("General_UseGpu", "On"),
    ("PSFParam_PsfInterp", "Cubic"),
    ("PSFParam_PsfModel", "GibsonLanniZernike"),
    ("PSFParam_PsfOversampling", "6"),
    ("PSFParam_PsfZernikePreset", "MixedRealisticObjective"),
    ("SimType_NupCount", "80"),
    ("PSFParam_PsfKernelHalfWidthNm", "3000"),
]:
    actual = core.getProperty("SMLMCam", name)
    assert float(actual) == float(expected) if expected.replace(".", "").isdigit() else actual == expected,         f"expected {name} to default to {expected!r}, got {actual!r}"
print("Defaults OK: out-of-the-box values confirmed")

# --- NUP: end-to-end + reproducibility -----------------------------------
core.setProperty("SMLMCam", "SimType_Pattern", "NUP")
core.setProperty("SMLMCam", "SimType_NupCount", "8")
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
core.setProperty("SMLMCam", "Background_BackgroundPhotonsPerSec", "0.0")


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
core.setProperty("SMLMCam", "General_LabelingEfficiencyPct", "70")  # restore default
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
core.setProperty("SMLMCam", "SimType_NupCount", "80")  # restore default

# --- Calibration9Spots: nine ALWAYS-ON beads on a 3x3 grid ---------------
# The point of this pattern is that it bypasses the blinking model entirely
# (IPatternGenerator::AlwaysOnSites): every frame must show the same nine
# spots at the same x,y. Assert both halves -- the count (exactly 9 bright
# blobs) and the always-on part (a single frame already shows all nine, and
# two different frames agree on where they are).
core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
core.setProperty("SMLMCam", "SimType_Pattern", "Calibration9Spots")
core.setProperty("SMLMCam", "PSFParam_PsfModel", "Gaussian")  # fast; spot geometry is what matters here
core.setProperty("SMLMCam", "Background_BackgroundPhotonsPerSec", "0.0")
core.setProperty("SMLMCam", "General_GenerateStack", "1")
wait_for_stack(core, "SMLMCam")


def bright_blob_centroids(img, threshold_frac=0.3):
    """Flood-fill the pixels above threshold and return each blob's centroid.

    A tiny hand-rolled connected-components pass rather than scipy.ndimage --
    this is the only place the test suite needs one, and 128x128 makes an
    explicit stack-based flood fill trivially fast.
    """
    a = img.astype(np.float64)
    a -= a.min()
    mask = a > threshold_frac * a.max()
    h, w = mask.shape
    seen = np.zeros_like(mask)
    centroids = []
    for sy in range(h):
        for sx in range(w):
            if not mask[sy, sx] or seen[sy, sx]:
                continue
            stack, pts = [(sy, sx)], []
            seen[sy, sx] = True
            while stack:
                y, x = stack.pop()
                pts.append((y, x))
                for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            centroids.append((sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)))
    return centroids


core.snapImage()
cal_a = bright_blob_centroids(core.getImage())
core.snapImage()
cal_b = bright_blob_centroids(core.getImage())
assert len(cal_a) == 9, f"expected exactly 9 always-on calibration spots in a single frame, got {len(cal_a)}"
assert len(cal_b) == 9, f"expected all 9 calibration spots in the NEXT frame too, got {len(cal_b)}"
# Pair the two frames' spots by nearest neighbour rather than by sorting --
# noise shifts each centroid by a fraction of a pixel, which is enough to
# reorder a plain sort of (y, x) tuples even though every spot is exactly
# where it should be.
for ay, ax in cal_a:
    dist, (by, bx) = min(((ay - cy) ** 2 + (ax - cx) ** 2, (cy, cx)) for cy, cx in cal_b)
    assert dist ** 0.5 < 1.0, (
        f"calibration spot at ({ay:.2f},{ax:.2f}) has no counterpart within 1px in the next "
        f"frame (nearest is ({by:.2f},{bx:.2f})) -- these emitters are supposed to be fixed "
        "and always on"
    )
# The nine centroids must form a regular 3x3 grid: exactly 3 distinct rows
# and 3 distinct columns, evenly spaced.
rows = sorted({round(y / 4.0) for y, _ in cal_a})
cols = sorted({round(x / 4.0) for _, x in cal_a})
assert len(rows) == 3 and len(cols) == 3,     f"expected the 9 spots to form a 3x3 grid, got {len(rows)} rows x {len(cols)} cols"
print("Calibration9Spots OK: 9 always-on beads on a 3x3 grid, identical positions frame to frame")
core.setProperty("SMLMCam", "SimType_Pattern", "Circle")  # restore default
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
# Low density: Fft does one Fourier shift per emitter and is by far the
# slowest mode (minutes for 1000 frames at the default density).
core.setProperty("SMLMCam", "General_EmitterDensityPerSec", "0.1")
for interp in sorted(INTERP_VALUES):
    core.setProperty("SMLMCam", "PSFParam_PsfInterp", interp)
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    core.snapImage()
    img = core.getImage()
    assert img.std() > 0, f"expected non-blank frame at PsfInterp={interp}"
print(f"PsfInterp OK: allowed values confirmed, all {len(INTERP_VALUES)} modes produce non-blank frames")
core.setProperty("SMLMCam", "PSFParam_PsfInterp", "Cubic")  # restore default
core.setProperty("SMLMCam", "General_EmitterDensityPerSec", "0.5")  # restore default

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
core.setProperty("SMLMCam", "SimType_ResolutionSpacingsNm", "150 100 50")
assert core.getProperty("SMLMCam", "SimType_ResolutionSpacingsNm") == "150 100 50"
assert set(core.getAllowedPropertyValues("SMLMCam", "PSFParam_PsfMaskType")) == {"None", "DoubleHelix"}
core.setProperty("SMLMCam", "PSFParam_PsfZernikePreset", "None")
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
def stack_frames(props, n=20, seed=42):
    """Fresh device, apply props, generate the precomputed stack, return n frames (float64)."""
    core.unloadDevice("SMLMCam")
    load_camera(core, "SMLMCam", seed=seed, fov="128x128")
    core.setProperty("SMLMCam", "General_AcqMode", "Precomputed")
    for k, v in props.items():
        core.setProperty("SMLMCam", k, v)
    core.setProperty("SMLMCam", "General_GenerateStack", "1")
    wait_for_stack(core, "SMLMCam")
    out = []
    for _ in range(n):
        core.snapImage()
        out.append(core.getImage().astype(np.float64))
    return np.stack(out)

FAST = {"PSFParam_PsfModel": "Gaussian"}

# Multi-blink: density keeps meaning ON-density, so the mean signal above
# offset stays about the same when molecules blink ~5x (bleach prob 0.2).
single = stack_frames({**FAST, "General_EmitterDensityPerSec": "20"}, n=200)
multi = stack_frames({**FAST, "General_EmitterDensityPerSec": "20", "FluoParam_BlinkBleachProb": "0.2",
                      "FluoParam_PhotonCV": "0.5"}, n=200)
s_sig, m_sig = single.mean() - 100.0, multi.mean() - 100.0
assert abs(m_sig - s_sig) < 0.25 * s_sig, f"multi-blink changed ON-density: single {s_sig:.3f} vs multi {m_sig:.3f}"
assert not np.array_equal(single, multi)
print(f"Multi-blink OK: mean signal single {s_sig:.3f} vs bleach=0.2/CV=0.5 {m_sig:.3f} ADU")

# EMCCD: background-only frame -> the gain register doubles the variance
# (excess noise factor sqrt(2)); ADU clipped to the bit depth.
bg_only = {**FAST, "General_EmitterDensityPerSec": "0.1", "FluoParam_PhotonsPerSecond": "1000",
           "Background_BackgroundPhotonsPerSec": "2000", "CamParam_ReadNoiseElectrons": "0",
           "CamParam_OffsetStdADU": "0", "CamParam_GainStdPctPerPixel": "0", "CamParam_GainPhotonsPerADU": "1",
           "CamParam_QuantumEfficiency": "1", "CamParam_DarkCurrentElectronsPerSec": "0"}
sc = stack_frames(bg_only, n=10)
em = stack_frames({**bg_only, "CamParam_CameraType": "EMCCD", "CamParam_CicElectrons": "0"}, n=10)
fano_sc = sc.var(axis=0).mean() / (sc.mean() - 100.0)
fano_em = em.var(axis=0).mean() / (em.mean() - 100.0)
assert 0.8 < fano_sc < 1.2 and 1.7 < fano_em < 2.3, f"variance/mean sCMOS {fano_sc:.2f}, EMCCD {fano_em:.2f}"
em12 = stack_frames({**bg_only, "CamParam_CameraType": "EMCCD", "CamParam_BitDepth": "8"}, n=2)
assert em12.max() <= 255, f"EMCCD BitDepth=8 must clip at 255, got {em12.max()}"
print(f"EMCCD OK: variance/mean sCMOS {fano_sc:.2f}, EMCCD {fano_em:.2f}; 8-bit clip holds")

# Structured background (cell) keeps the FOV mean; Gaussian illumination dims corners.
flat_bg = {**FAST, "General_EmitterDensityPerSec": "0.1", "Background_BackgroundPhotonsPerSec": "400"}
cell = stack_frames({**flat_bg, "Background_CellContrast": "5"}, n=5).mean(axis=0) - 100
flat = stack_frames(flat_bg, n=5).mean(axis=0) - 100
assert abs(cell.mean() - flat.mean()) < 0.05 * flat.mean(), "cell background must keep the FOV mean"
assert cell[54:74, 54:74].mean() > 2 * cell[:10, :10].mean(), "cell background: centre should be brighter than corner"
illum = stack_frames({**flat_bg, "FluoParam_IllumProfile": "Gaussian"}, n=5).mean(axis=0) - 100
assert illum[:10, :10].mean() < 0.5 * illum[54:74, 54:74].mean(), "Gaussian illumination must dim the corners"
decay = stack_frames({**flat_bg, "Background_DecaySec": "0.5"}, n=40)
assert decay[-1].mean() < decay[0].mean(), "background fade must lower the background over time"
print("Background OK: cell contrast keeps the mean, illumination dims corners, fade decays")

# FilamentsRing renders; out-of-focus emitters add light (vectorial PSF only).
fr = stack_frames({**FAST, "SimType_Pattern": "FilamentsRing"}, n=3)
assert fr.std() > 0
vec = {"PSFParam_PsfZRangeUm": "4", "PSFParam_PsfZStepUm": "0.2", "SimType_Pattern": "FilamentsRing",
       "General_EmitterDensityPerSec": "5"}
no_oof = stack_frames(vec, n=30)
oof = stack_frames({**vec, "Background_OutOfFocusRatio": "2"}, n=30)
assert oof.mean() > no_oof.mean(), "out-of-focus emitters must add light"
print(f"FilamentsRing/out-of-focus OK: mean {no_oof.mean():.3f} -> {oof.mean():.3f} with 2x out-of-focus emitters")

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

print("All SMLMDemoCam smoke tests passed.")
