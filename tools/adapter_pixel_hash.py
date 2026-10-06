"""Pixel hash of the inSiliScope adapter's precomputed output for fixed seeds.

Used to prove a refactor did not change the image output: run it against the
DLL built before and after, and compare the printed hashes.

    python tools/adapter_pixel_hash.py <dir containing mmgr_dal_inSiliScope.dll> [--frames N]

The DLL is loaded from that directory (MMCore's adapter search path lists it
first); the active pymmcore-plus Micro-Manager install (or MM_DIR) provides
the rest. Nothing is copied into the MM install. CPU render path only, so the
hash does not depend on the GPU.

The adapter is the inSiliScope hub with its devices (spec/MM_DEVICES.md); the
precomputed stack is a Test row (the environment variable ISC_TEST=1, set
here). The script also drives the pre-hub layout (one Camera with every
property, before 2026-10-06), mapping each setting to its old name, so a hash
of main before the split can be compared with one after it.
"""

import argparse
import hashlib
import os
import sys
import time

os.environ["ISC_TEST"] = "1"   # the Test rows (precomputed stack) exist only with this

from pymmcore_plus import CMMCorePlus
from pymmcore_plus._util import USER_DATA_DIR

GAUSS = [("Renderer", "PsfModel", "Gaussian")]
# (name, pre-init seed, [(device, property, value)]). Each config regenerates
# the precomputed stack and hashes its first frames. The defaults are DNA-PAINT
# ATTO 655 with its imager background.
CONFIGS = [
    ("defaults", 1234, []),
    ("gaussian-psf", 1234, GAUSS),
    ("dstorm", 99, GAUSS + [("CellField", "Microtubules_Dye", "AF647")]),
    ("palm", 11, GAUSS + [("CellField", "Microtubules_Dye", "mEos3.2")]),
    ("emccd", 7, [("Camera", "CameraType", "EMCCD")] + GAUSS),
    ("illum-bg", 3, GAUSS + [("Lasers", "IlluminationProfile", "Gaussian"),
                             ("SampleHolder", "BackgroundPhotonsPerSec", "200"),
                             ("SampleHolder", "BackgroundDecaySec", "0.5")]),
    ("mean-field", 5, [("CellField", "Microtubules_Dye", "mEGFP")]),
    ("brightfield", 21, [("light", "", "BrightField")]),
    ("drift", 31, GAUSS + [("SampleHolder", "DriftXySpeedNmPerSec", "200"),
                           ("SampleHolder", "DriftXyNmPerSqrtSec", "20"),
                           ("SampleHolder", "DriftZNmPerSqrtSec", "10")]),
    ("zernike", 41, [("Objective", "ZernikePreset", "AstigmatismStrong")]),
    ("dstorm-ixon", 51, GAUSS + [("CellField", "Microtubules_Dye", "AF647"),
                                 ("Camera", "CameraPreset", "iXonUltra897"),
                                 ("Lasers", "Preset", "dSTORM-640")]),
    ("cells-na", 61, GAUSS + [("CellField", "CellDiameterMinUm", "15"), ("CellField", "Occupancy", "0.6"),
                              ("Objective", "NA", "1.2"), ("CellField", "Microtubules_LabelingPct", "30"),
                              ("CellField", "Microtubules_ImagerNm", "5")]),
]

# The pre-hub layout: (device, property) -> the camera's old property name.
OLD = {
    ("Renderer", "PsfModel"): "PSFParam_PsfModel",
    ("Renderer", "UseGpu"): "General_UseGpu",
    ("CellField", "Microtubules_Dye"): "SimType_CellFieldMicrotubuleDye",
    ("CellField", "Microtubules_LabelingPct"): "SimType_CellFieldMicrotubuleLabelingPct",
    ("CellField", "Microtubules_ImagerNm"): "SimType_CellFieldMicrotubuleImagerNm",
    ("CellField", "CellDiameterMinUm"): "SimType_CellFieldCellDiameterMinUm",
    ("CellField", "Occupancy"): "SimType_CellFieldOccupancy",
    ("Camera", "CameraType"): "CamParam_CameraType",
    ("Camera", "CameraPreset"): "CamParam_CameraPreset",
    ("Lasers", "IlluminationProfile"): "Optics_IlluminationProfile",
    ("Lasers", "Preset"): "Optics_Preset",
    ("SampleHolder", "BackgroundPhotonsPerSec"): "Background_BackgroundPhotonsPerSec",
    ("SampleHolder", "BackgroundDecaySec"): "Background_DecaySec",
    ("SampleHolder", "DriftXySpeedNmPerSec"): "SimType_DriftXySpeedNmPerSec",
    ("SampleHolder", "DriftXyNmPerSqrtSec"): "SimType_DriftXyNmPerSqrtSec",
    ("SampleHolder", "DriftZNmPerSqrtSec"): "SimType_DriftZNmPerSqrtSec",
    ("Objective", "ZernikePreset"): "PSFParam_PsfZernikePreset",
    ("Objective", "NA"): "PSFParam_PsfNa",
}
PERIPHERALS = ["Camera", "XYStage", "ZStage", "Objective", "EmissionPath", "FilterCube", "Lasers", "TransmittedLamp",
               "SampleHolder", "CellField", "Fluorophores", "Renderer"]


def find_mm_dir() -> str:
    if os.environ.get("MM_DIR"):
        return os.environ["MM_DIR"]
    candidates = sorted((USER_DATA_DIR / "mm").glob("Micro-Manager_*"))
    candidates = [c for c in candidates if c.is_dir()]
    if not candidates:
        sys.exit("No Micro-Manager install found; run `mmcore install` or set MM_DIR")
    return str(candidates[-1])


def set_checked(core, label, prop, value):
    allowed = core.getAllowedPropertyValues(label, prop)
    if allowed and value not in allowed:
        sys.exit(f"{label}.{prop}={value} not allowed; allowed: {allowed}")
    core.setProperty(label, prop, value)


def wait_for_stack(core, label, status_prop, timeout_s=900.0):
    t0 = time.time()
    while True:
        status = core.getProperty(label, status_prop)
        if status.startswith("Ready"):
            return
        if time.time() - t0 > timeout_s:
            sys.exit(f"timed out waiting for stack, last status: {status}")
        time.sleep(0.1)


def load_hub_layout(core, seed):
    """The hub and its devices; returns the stack property names."""
    core.loadDevice("Hub", "inSiliScope", "inSiliScope")
    core.setProperty("Hub", "Detail", "Expert")
    core.setProperty("Hub", "RandomSeed", str(seed))
    for d in PERIPHERALS:
        core.loadDevice(d, "inSiliScope", d)
        core.setParentLabel(d, "Hub")
    core.setProperty("Camera", "FovSize", "128x128")
    core.initializeAllDevices()
    core.setCameraDevice("Camera")
    core.setAutoShutter(False)
    core.setProperty("Lasers", "State", "1")   # the epi light on (fluorescence)
    core.setPosition("ZStage", 0.0)            # the coverslip in focus (the old layout had no ZStage)
    set_checked(core, "Renderer", "UseGpu", "Off")
    return "Camera", "Test_AcqMode", "Test_GenerateStack", "Test_StackGenerationStatus"


def load_old_layout(core, seed):
    core.loadDevice("Camera", "inSiliScope", "Camera")
    core.setProperty("Camera", "SimType_RandomSeed", str(seed))
    core.initializeDevice("Camera")
    core.setCameraDevice("Camera")
    set_checked(core, "Camera", "General_UseGpu", "Off")
    set_checked(core, "Camera", "General_FovSize", "128x128")
    return "Camera", "General_AcqMode", "General_GenerateStack", "General_StackGenerationStatus"


def apply(core, hub_layout, settings):
    for device, prop, value in settings:
        if device == "light":   # BrightField: the lamp alone
            if hub_layout:
                core.setProperty("Lasers", "State", "0")
                core.setProperty("TransmittedLamp", "State", "1")
            else:
                set_checked(core, "Camera", "General_ImagingModality", value)
        elif hub_layout:
            set_checked(core, device, prop, value)
        else:
            set_checked(core, "Camera", OLD[(device, prop)], value)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("adapter_dir")
    ap.add_argument("--frames", type=int, default=5)
    args = ap.parse_args()

    total = hashlib.sha256()
    for name, seed, props in CONFIGS:
        core = CMMCorePlus()
        core.setDeviceAdapterSearchPaths([os.path.abspath(args.adapter_dir), find_mm_dir()])
        hub_layout = "inSiliScope" in core.getAvailableDevices("inSiliScope")
        cam, acq, gen, status = (load_hub_layout if hub_layout else load_old_layout)(core, seed)
        apply(core, hub_layout, props)
        set_checked(core, cam, acq, "Precomputed")
        t0 = time.time()
        core.setProperty(cam, gen, "1")
        wait_for_stack(core, cam, status)
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
