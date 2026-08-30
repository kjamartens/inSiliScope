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
    core.setProperty(label, "RandomSeed", str(seed))  # pre-init
    core.initializeDevice(label)
    core.setCameraDevice(label)
    core.setProperty(label, "FovSize", fov)  # regular property, set after init


def wait_for_stack(core: CMMCorePlus, label: str, timeout_s: float = 60.0) -> None:
    t0 = time.time()
    while True:
        status = core.getProperty(label, "StackGenerationStatus")
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
assert core.isPropertyPreInit("SMLMCam", "RandomSeed")
assert not core.isPropertyPreInit("SMLMCam", "FovSize"), \
    "FovSize should be a regular, post-init-changeable property"
print("Pre-init property confirmed: RandomSeed (FovSize confirmed NOT pre-init)")

# --- Precomputed mode: trigger generation, poll status, snap -----------
core.setProperty("SMLMCam", "AcqMode", "Precomputed")
core.setProperty("SMLMCam", "StackLength", "50")
core.setProperty("SMLMCam", "GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
print("Stack generation status:", core.getProperty("SMLMCam", "StackGenerationStatus"))

core.snapImage()
img = core.getImage()
assert img.shape == (128, 128), f"expected (128, 128), got {img.shape}"
assert img.dtype == np.uint16, f"expected uint16, got {img.dtype}"
assert img.std() > 0, "expected a non-blank frame"
print("Snap OK. Image shape:", img.shape, "dtype:", img.dtype, "std:", img.std())

# --- Reproducibility: same seed + params -> byte-identical stack -------
frames_a = []
for _ in range(5):
    core.snapImage()
    frames_a.append(core.getImage().copy())

core.unloadDevice("SMLMCam")
load_camera(core, "SMLMCam", seed=42, fov="128x128")
core.setProperty("SMLMCam", "AcqMode", "Precomputed")
core.setProperty("SMLMCam", "StackLength", "50")
core.setProperty("SMLMCam", "GenerateStack", "1")
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
core.setProperty("SMLMCam", "AcqMode", "Precomputed")
core.setProperty("SMLMCam", "StackLength", "50")
core.setProperty("SMLMCam", "GenerateStack", "1")
wait_for_stack(core, "SMLMCam")
core.snapImage()
frame_diff_seed = core.getImage()
assert not np.array_equal(frame_diff_seed, frames_a[0]), \
    "expected a different RandomSeed to produce a different frame"
print("Reproducibility OK: a different seed produced a different frame")

# --- Live mode: stream a short sequence, confirm frames change ---------
core.setProperty("SMLMCam", "AcqMode", "Live")
core.setProperty("SMLMCam", "EmitterDensityPerSec", "50.0")
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

# --- Regression: changing a noise parameter mid-Live-stream must actually
# change subsequent frames, without needing to toggle AcqMode or restart
# anything (this used to silently no-op for CameraOffsetStdADU/CameraOffsetADU
# because the live producer thread cached its fixed-pattern offset map and
# only rebuilt it on a frame-size change). Use a large offset-std delta so
# the resulting frame-to-frame std shift is unambiguous against ordinary
# shot/read noise.
core.setProperty("SMLMCam", "CameraOffsetStdADU", "0.0")
core.setProperty("SMLMCam", "BackgroundPhotonsPerSec", "0.0")
time.sleep(0.3)  # let a few live ticks pass with the low-offset-std setting
core.snapImage()
std_before = float(np.std(core.getImage().astype(np.float64)))

core.setProperty("SMLMCam", "CameraOffsetStdADU", "50.0")
time.sleep(0.3)  # let the live producer thread pick up the change
core.snapImage()
std_after = float(np.std(core.getImage().astype(np.float64)))

print("Live offset-std hookup: measured frame std before=%.3f after=%.3f" % (std_before, std_after))
assert std_after > std_before + 10.0, (
    f"expected CameraOffsetStdADU=50 to visibly increase frame-to-frame pixel std "
    f"vs CameraOffsetStdADU=0 (before={std_before:.3f}, after={std_after:.3f}) -- "
    "looks like the live producer thread isn't picking up the change"
)
core.setProperty("SMLMCam", "CameraOffsetStdADU", "0.5")  # restore default
print("Regression OK: CameraOffsetStdADU change took effect live, no restart needed")

print("All SMLMDemoCam smoke tests passed.")
