"""Writes the shipped Micro-Manager configurations adapter/inSiliScope/config/inSiliScope_{Basic,Advanced,Expert}.cfg.

Generated, never hand-edited (spec/MM_DEVICES.md): the devices and their state labels come from the adapter itself
(loaded through pymmcore-plus, so ADAPTER_DIR must point at a built adapter), the Channel presets from the typical
labels of data/dyes/library.json, the pixel sizes from the cameras' sensor pixels in data/dyes/cameras.json.

  ADAPTER_DIR=<dll dir> python tools/gen_mm_configs.py          # write the three files
  ADAPTER_DIR=<dll dir> python tools/gen_mm_configs.py --check  # fail if they are out of date

tools/test_mm_configs.py loads each file and checks its groups, pixel sizes and snaps.
"""

import json
import os
import sys
from pathlib import Path

os.environ["ISC_TEST"] = "0"   # the user's view: no Test rows
sys.path.insert(0, str(Path(__file__).resolve().parent))
from isc_mm import ALL_DEVICES, HUB, search_paths  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "adapter" / "inSiliScope" / "config"
TIERS = ("Basic", "Advanced", "Expert")
WHEELS = ("ExcitationFilter", "Dichroic", "EmissionFilter")   # Advanced devices
MULTI = "Multi Shutter"   # Utilities: both light sources at once (Advanced, Expert)
FOV = "256x256"
SEED = "42"
# The relay that goes with each camera (EmissionPath): the Kinetix22's 6.5 um pixel at 100x x 0.667 = 97.45 nm, the
# iXon's 16 um at 100x x 1.6 = 100 nm (the ~100 nm of SMLM setups).
CAMERA_RELAY = {"Kinetix22": 0.667, "iXonUltra897": 1.6}
QUALITY = ("Fast", "Realistic", "Exhaustive")
# Laser lines with a power group: one property and one preset each, which Micro-Manager Studio shows as an editable
# field (a slider within the property's limits) in its main window, so the power is one click away (2026-10-13).
LASER_GROUP_NM = (561, 640)
STARTUP_MODE = "DNA-PAINT"   # the engine's default label (ATTO655): the PSF preload at init is the first frame's kernel


def preset_name(label):
    """A state label as a preset name (MM reserves '/' there): 100x/1.40 Oil -> 100x NA 1.40 Oil."""
    return label.replace("/", " NA ")


def num(v):
    """A number the way the adapter reports it (MM matches presets by string)."""
    return f"{float(v):.4f}"


def devices_for(tier):
    return [d for d in ALL_DEVICES if tier != "Basic" or d not in WHEELS]


def read_adapter():
    """The state labels and magnifications the adapter offers (Expert, every device)."""
    from pymmcore_plus import CMMCorePlus

    core = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(search_paths())
    core.loadDevice(HUB, "inSiliScope", "inSiliScope")
    core.setProperty(HUB, "Detail", "Expert")
    for d in ALL_DEVICES:
        core.loadDevice(d, "inSiliScope", d)
        core.setParentLabel(d, HUB)
    core.setProperty("Camera", "FovSize", "128x128")
    core.initializeAllDevices()
    objectives = []
    for label in core.getStateLabels("Objective"):
        if label == "Custom":
            continue
        core.setProperty("Objective", "Label", label)
        objectives.append((label, float(core.getProperty("Objective", "Magnification"))))
    info = {
        "objectives": objectives,
        "defaultObjective": objectives[0][0],
        "cameras": [c for c in core.getAllowedPropertyValues("Camera", "CameraPreset") if c != "Custom"],
        "modes": list(core.getAllowedPropertyValues("CellField", "Microtubules_Mode")),
        "specimens": list(core.getStateLabels("SampleHolder")),
        "quality": list(core.getAllowedPropertyValues("Renderer", "Quality")),
        # SampleHolder.DriftPreset (Basic): Off ... Extreme; Custom is no preset
        "drift": [d for d in core.getAllowedPropertyValues("SampleHolder", "DriftPreset") if d != "Custom"],
        # the startup powers (the Lasers' Auto preset for the default labels), kW/cm^2
        "lasers": {nm: float(core.getProperty("Lasers", laser_prop(nm))) for nm in LASER_GROUP_NM},
    }
    core.unloadAllDevices()
    return info


def laser_prop(nm):
    return f"Laser{nm}KWcm2"


def laser_group(nm):
    return f"Laser {nm} nm (kW/cm2)"


def channel_presets(tier, typical, modes):
    """Channel: one preset per mode with every target in it and its typical label (the cell field's microtubules and
    nuclear pores, the nanorulers' docking strands; lasers and filters follow the mounted specimen's labels: the Lasers'
    Auto preset), BrightField, and
    (Advanced, Expert) fluorescence + BrightField through the Multi Shutter. Every preset closes both shutters; the
    autoshutter opens the Core-Shutter for each image."""
    closed = [("Lasers", "State", "0"), ("TransmittedLamp", "State", "0")]
    out = []
    order = [m for m in ("dSTORM", "PALM", "DNA-PAINT", "WideField") if m in modes]
    for mode in order:
        out.append((f"{typical[mode]['dye']} {mode}", every_target(mode) + closed + [("Core", "Shutter", "Lasers")]))
    out.append(("BrightField", closed + [("Core", "Shutter", "TransmittedLamp")]))
    if tier != "Basic":
        out.append((f"{typical['WideField']['dye']} WideField + BrightField",
                    every_target("WideField") + closed + [("Core", "Shutter", MULTI)]))
    return out


# Every target in one mode with its typical label (data/specimens.json's targets: the device and row of each).
TARGET_ROWS = [("CellField", "Microtubules"), ("CellField", "NuclearPores"), ("Nanorulers", "Docking")]


def every_target(mode):
    return [p for dev, row in TARGET_ROWS for p in ((dev, row + "_Mode", mode), (dev, row + "_Label", "Typical"))]


def build(tier, info, typical, cameras):
    devs = devices_for(tier)
    L = []
    w = L.append
    w("# inSiliScope, Detail = %s: generated by tools/gen_mm_configs.py from the adapter and data/ -- do not edit." % tier)
    w("# The simulated microscope (spec/MM_DEVICES.md, docs/mm-properties.md). Groups: Channel (the label mode and its")
    w("# typical dye; lasers and filter cube follow), Objective, Camera (sensor + its relay), Quality, Drift, Specimen,")
    w("# and the 561 and 640 nm laser powers (one property each: Studio shows an editable field).")
    w("")
    w("# Reset")
    w("Property,Core,Initialize,0")
    w("")
    w("# Devices")
    w(f"Device,{HUB},inSiliScope,inSiliScope")
    for d in devs:
        w(f"Device,{d},inSiliScope,{d}")
    if tier != "Basic":
        w(f"Device,{MULTI},Utilities,{MULTI}")
    w("")
    w("# Pre-init settings for devices")
    w(f"Property,{HUB},Detail,{tier}")
    w(f"Property,{HUB},RandomSeed,{SEED}")
    w(f"Property,Camera,FovSize,{FOV}")
    w("")
    w("# Hub (parent) references")
    for d in devs:
        w(f"Parent,{d},{HUB}")
    w("")
    w("# Initialize")
    w("Property,Core,Initialize,1")
    w("")
    w("# Focus directions (+Z moves the focal plane up through the sample)")
    w("FocusDirection,ZStage,1")
    w("")
    w("# Roles")
    w("Property,Core,Camera,Camera")
    w("Property,Core,Shutter,Lasers")
    w("Property,Core,Focus,ZStage")
    w("Property,Core,XYStage,XYStage")
    w("Property,Core,AutoShutter,1")
    w("")

    groups = []
    channels = channel_presets(tier, typical, info["modes"])
    groups.append(("Channel", channels))
    groups.append(("Objective", [(preset_name(label), [("Objective", "Label", label)])
                                 for label, _ in info["objectives"]]))
    groups.append(("Camera", [(c, [("Camera", "CameraPreset", c), ("EmissionPath", "EmissionMagnification",
                                                                     num(CAMERA_RELAY[c]))]) for c in cameras]))
    groups.append(("Quality", [(q, [("Renderer", "Quality", q)]) for q in QUALITY if q in info["quality"]]))
    groups.append(("Drift", [(d, [("SampleHolder", "DriftPreset", d)]) for d in info["drift"]]))
    groups.append(("Specimen", [(s, [("SampleHolder", "Label", s)]) for s in info["specimens"]]))
    # One property, one preset ('NewPreset', as Studio's group editor names it): Studio shows the power as a field.
    for nm in LASER_GROUP_NM:
        groups.append((laser_group(nm), [("NewPreset", [("Lasers", laser_prop(nm), num(info["lasers"][nm]))])]))

    startup = [("Core", "ChannelGroup", "Channel")]
    if tier != "Basic":
        startup += [(MULTI, "Physical Shutter 1", "Lasers"), (MULTI, "Physical Shutter 2", "TransmittedLamp")]
    first = dict(groups)
    startup += dict(first["Channel"])[f"{typical[STARTUP_MODE]['dye']} {STARTUP_MODE}"]
    startup += dict(first["Objective"])[preset_name(info["defaultObjective"])]
    startup += dict(first["Camera"])[cameras[0]]
    startup += dict(first["Quality"])["Realistic"]
    startup += dict(first["Drift"])["Off"]
    startup += dict(first["Specimen"])["CellField"]

    w("# Configuration presets")
    w("# Group: System")
    w("# Preset: Startup")
    for dev, prop, val in startup:
        w(f"ConfigGroup,System,Startup,{dev},{prop},{val}")
    w("")
    for name, presets in groups:
        w("")
        w(f"# Group: {name}")
        for preset, settings in presets:
            w(f"# Preset: {preset}")
            for dev, prop, val in settings:
                w(f"ConfigGroup,{name},{preset},{dev},{prop},{val}")
            w("")
    w("")
    w("# PixelSize settings: the sensor pixel / the objective (MM divides by the EmissionPath magnifier)")
    for label, mag in info["objectives"]:
        for c in cameras:
            res = f"{preset_name(label)} {c}"
            w(f"# Resolution preset: {res}")
            w(f"ConfigPixelSize,{res},Objective,Label,{label}")
            w(f"ConfigPixelSize,{res},Camera,CameraPreset,{c}")
            w(f"PixelSize_um,{res},{SENSOR_UM[c] / mag:.12g}")
            w(f"PixelSizeAffine,{res},1.0,0.0,0.0,0.0,1.0,0.0")
            w("")
    return "\n".join(L) + "\n"


SENSOR_UM = {}   # camera preset -> sensor pixel (um), from cameras.json (load_sensors)


def load_sensors():
    cams = json.loads((ROOT / "data" / "dyes" / "cameras.json").read_text(encoding="utf-8"))
    for p in cams["presets"]:
        if p.get("physicalPixelUm"):
            SENSOR_UM[p["id"]] = float(p["physicalPixelUm"])


def main():
    check = "--check" in sys.argv
    lib = json.loads((ROOT / "data" / "dyes" / "library.json").read_text(encoding="utf-8"))
    load_sensors()
    typical = lib["typicalLabels"]["microtubules"]
    info = read_adapter()
    cameras = [c for c in info["cameras"] if c in CAMERA_RELAY]
    missing = set(info["cameras"]) - set(CAMERA_RELAY)
    if missing:
        sys.exit(f"gen_mm_configs: no relay magnification for camera preset(s) {sorted(missing)} (CAMERA_RELAY)")
    stale = []
    OUT.mkdir(parents=True, exist_ok=True)
    for tier in TIERS:
        text = build(tier, info, typical, cameras)
        path = OUT / f"inSiliScope_{tier}.cfg"
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {path.relative_to(ROOT)}")
    if stale:
        sys.exit(f"out of date (run tools/gen_mm_configs.py): {', '.join(stale)}")
    if check:
        print("MM configs up to date")


if __name__ == "__main__":
    main()
