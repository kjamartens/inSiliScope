"""Pixel hash of the inSiliScope adapter's precomputed output for fixed seeds.

Used to prove a refactor did not change the image output: run it against the
DLL built before and after, and compare the printed hashes.

    python tools/adapter_pixel_hash.py <dir containing mmgr_dal_inSiliScope.dll> [--frames N]

The DLL is loaded from that directory (MMCore's adapter search path lists it
first); the active pymmcore-plus Micro-Manager install (or MM_DIR) provides
the rest. Nothing is copied into the MM install. CPU render path only, so the
hash does not depend on the GPU.
"""

import argparse
import hashlib
import os
import sys
import time

from pymmcore_plus import CMMCorePlus
from pymmcore_plus._util import USER_DATA_DIR

# (name, pre-init seed, post-init properties). Each config regenerates the
# precomputed stack and hashes its first frames. CellField only (the other
# patterns were removed with issue 16); the defaults are DNA-PAINT ATTO 655
# with its imager background (issue 16, phase 3: the hashes changed then).
GAUSS = {"PSFParam_PsfModel": "Gaussian"}
CONFIGS = [
    ("defaults", 1234, {}),
    ("gaussian-psf", 1234, GAUSS),
    ("dstorm", 99, {**GAUSS, "SimType_CellFieldMicrotubuleDye": "AF647"}),
    ("palm", 11, {**GAUSS, "SimType_CellFieldMicrotubuleDye": "mEos3.2"}),
    ("emccd", 7, {"CamParam_CameraType": "EMCCD", **GAUSS}),
    ("illum-bg", 3, {**GAUSS, "Optics_IlluminationProfile": "Gaussian",
                     "Background_BackgroundPhotonsPerSec": "200", "Background_DecaySec": "0.5"}),
    ("mean-field", 5, {"SimType_CellFieldMicrotubuleDye": "mEGFP"}),
]


def find_mm_dir() -> str:
    if os.environ.get("MM_DIR"):
        return os.environ["MM_DIR"]
    candidates = sorted((USER_DATA_DIR / "mm").glob("Micro-Manager_*"))
    candidates = [c for c in candidates if c.is_dir()]
    if not candidates:
        sys.exit("No Micro-Manager install found; run `mmcore install` or set MM_DIR")
    return str(candidates[-1])


def wait_for_stack(core, label, timeout_s=900.0):
    t0 = time.time()
    while True:
        status = core.getProperty(label, "General_StackGenerationStatus")
        if status.startswith("Ready"):
            return
        if time.time() - t0 > timeout_s:
            sys.exit(f"timed out waiting for stack, last status: {status}")
        time.sleep(0.1)


def set_checked(core, label, prop, value):
    allowed = core.getAllowedPropertyValues(label, prop)
    if allowed and value not in allowed:
        sys.exit(f"{prop}={value} not allowed; allowed: {allowed}")
    core.setProperty(label, prop, value)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("adapter_dir")
    ap.add_argument("--frames", type=int, default=5)
    args = ap.parse_args()

    total = hashlib.sha256()
    for name, seed, props in CONFIGS:
        core = CMMCorePlus()
        core.setDeviceAdapterSearchPaths([os.path.abspath(args.adapter_dir), find_mm_dir()])
        core.loadDevice("Cam", "inSiliScope", "Camera")
        core.setProperty("Cam", "SimType_RandomSeed", str(seed))
        core.initializeDevice("Cam")
        core.setCameraDevice("Cam")
        set_checked(core, "Cam", "General_UseGpu", "Off")
        set_checked(core, "Cam", "General_FovSize", "128x128")
        for k, v in props.items():
            set_checked(core, "Cam", k, v)
        set_checked(core, "Cam", "General_AcqMode", "Precomputed")
        t0 = time.time()
        core.setProperty("Cam", "General_GenerateStack", "1")
        wait_for_stack(core, "Cam")
        h = hashlib.sha256()
        for _ in range(args.frames):
            core.snapImage()
            img = core.getImage()
            h.update(str(img.shape).encode() + str(img.dtype).encode() + img.tobytes())
        total.update(h.digest())
        print(f"{name:14s} {h.hexdigest()}  ({time.time() - t0:.1f} s)", flush=True)
        core.unloadAllDevices()
    print(f"{'TOTAL':14s} {total.hexdigest()}")


if __name__ == "__main__":
    main()
