"""Loading the inSiliScope Micro-Manager devices in pymmcore-plus, for the test scripts.

The adapter is a hub with peripherals (spec/MM_DEVICES.md): load_scope() loads the hub and its devices, each labelled
with its device name, under the given Detail. ISC_TEST=1 (set on import, before any adapter loads) creates the Test
rows: the precomputed, seeded stack the reproducibility checks use (Camera Test_AcqMode, Test_GenerateStack, ...).
"""

import os
import sys
import time

os.environ.setdefault("ISC_TEST", "1")

from pymmcore_plus._util import USER_DATA_DIR

HUB = "Hub"
ALL_DEVICES = ["Camera", "XYStage", "ZStage", "Objective", "EmissionPath", "FilterCube", "ExcitationFilter", "Dichroic",
               "EmissionFilter",
               "Lasers", "TransmittedLamp", "SampleHolder", "CellField", "Fluorophores", "Renderer"]


def find_mm_dir() -> str:
    if os.environ.get("MM_DIR"):
        return os.environ["MM_DIR"]
    candidates = [c for c in sorted((USER_DATA_DIR / "mm").glob("Micro-Manager_*")) if c.is_dir()]
    if not candidates:
        sys.exit("No Micro-Manager install found; run `mmcore install` or set MM_DIR")
    return str(candidates[-1])


def search_paths():
    return [d for d in (os.environ.get("ADAPTER_DIR"), os.environ.get("MM_DIR") or find_mm_dir()) if d]


def load_scope(core, seed=42, fov="128x128", detail="Expert", devices=ALL_DEVICES, epi=True, trans=False):
    """The hub and its devices (labels = device names), initialized; camera, stages and shutter roles set. The light:
    the lasers open (fluorescence) unless epi is False, the lamp open if trans; autoshutter off."""
    core.loadDevice(HUB, "inSiliScope", "inSiliScope")
    core.setProperty(HUB, "Detail", detail)
    core.setProperty(HUB, "RandomSeed", str(seed))
    for d in devices:
        core.loadDevice(d, "inSiliScope", d)
        core.setParentLabel(d, HUB)
    if "Camera" in devices:
        core.setProperty("Camera", "FovSize", fov)
    core.initializeAllDevices()
    if "Camera" in devices:
        core.setCameraDevice("Camera")
    if "XYStage" in devices:
        core.setXYStageDevice("XYStage")
    if "ZStage" in devices:
        core.setFocusDevice("ZStage")
    core.setAutoShutter(False)
    set_light(core, epi, trans)


def set_light(core, epi, trans):
    """The light sources' shutters: the lasers (epi, fluorescence) and the lamp (transmitted, BrightField)."""
    loaded = core.getLoadedDevices()
    if "Lasers" in loaded:
        core.setProperty("Lasers", "State", "1" if epi else "0")
    if "TransmittedLamp" in loaded:
        core.setProperty("TransmittedLamp", "State", "1" if trans else "0")


def brightfield(core, on=True):
    """BrightField = the lamp alone; fluorescence = the lasers alone."""
    set_light(core, not on, on)


def wait_for_stack(core, timeout_s=600.0):
    t0 = time.time()
    while not core.getProperty("Camera", "Test_StackGenerationStatus").startswith("Ready"):
        if time.time() - t0 > timeout_s:
            sys.exit("timed out waiting for the precomputed stack: "
                     + core.getProperty("Camera", "Test_StackGenerationStatus"))
        time.sleep(0.05)


def generate_stack(core):
    core.setProperty("Camera", "Test_GenerateStack", "1")
    wait_for_stack(core)


def precomputed(core, on=True):
    core.setProperty("Camera", "Test_AcqMode", "Precomputed" if on else "Live")


def label(core, mode, dye="Typical"):
    """The microtubules' label: the experiment's mode (Fluorophores), then their dye (Typical, or one with data in it)."""
    core.setProperty("Fluorophores", "Mode", mode)
    core.setProperty("CellField", "Microtubules_Label", dye)


def custom_microtubule_dye(core, **fields):
    """Edits of the microtubules' dye: a custom dye (Fluorophores Dye1, Expert) started from the library dye their label
    resolves to, with these fields (property suffixes, e.g. InitialOnSec=0), labels the microtubules."""
    dye = core.getProperty("Fluorophores", "Microtubules_EffectiveDye").split(" (")[0]
    if not core.getProperty("CellField", "Microtubules_Label").startswith("Dye"):
        # A new source clears the slot's edits (set another first, so the same dye counts as new too).
        core.setProperty("Fluorophores", "Dye1_Source", "Custom" if dye != "Custom" else "AF647")
        core.setProperty("Fluorophores", "Dye1_Source", dye)
        core.setProperty("CellField", "Microtubules_Label", "Dye1")
    for k, v in fields.items():
        core.setProperty("Fluorophores", "Dye1_" + k, str(v))


def timed_prints():
    """Every line printed carries the seconds since the previous one: what each check costs."""
    import builtins
    t = [time.time()]
    orig = builtins.print

    def p(*a, **k):
        now = time.time()
        orig(f"[{now - t[0]:6.1f} s]", *a, **k)
        t[0] = now

    builtins.print = p
