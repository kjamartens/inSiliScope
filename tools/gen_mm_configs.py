"""Writes the shipped Micro-Manager configurations adapter/inSiliScope/config/inSiliScope_{Basic,Advanced,Expert}.cfg.

Generated, never hand-edited (spec/MM_DEVICES.md): the devices and their state labels come from the adapter itself
(loaded through pymmcore-plus, so ADAPTER_DIR must point at a built adapter), the Channel presets from the typical
labels of data/dyes/library.json, the pixel sizes from the cameras' sensor pixels in data/dyes/cameras.json.

  ADAPTER_DIR=<dll dir> python tools/gen_mm_configs.py          # write the three files
  ADAPTER_DIR=<dll dir> python tools/gen_mm_configs.py --check  # fail if they are out of date

tools/test_mm_configs.py loads each file and checks its groups, pixel sizes and snaps.

The same run writes the viewer's copy, web/live/mm_config.js (the live panel's Configuration): each new file loaded in
the adapter, every preset of every group applied in turn (from the Startup preset) and the engine options it leaves set
(the engine spec the adapter renders with, Renderer.WriteScopeSpecTo), so the viewer's presets do what Micro-Manager's do, couplings
included (camera and drift presets, Quality, Typical labels, the light the shutters let through). --check covers it.
"""

import json
import os
import re
import sys
from pathlib import Path

os.environ["ISC_TEST"] = "0"   # the user's view: no Test rows
sys.path.insert(0, str(Path(__file__).resolve().parent))
from isc_mm import ALL_DEVICES, HUB, search_paths  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "adapter" / "inSiliScope" / "config"
VIEWER_OUT = ROOT / "web" / "live" / "mm_config.js"
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


def illumination_presets():
    """The Illumination group (2026-10-13): Lasers.BfpPosition at epi, the DNA-PAINT light presets' HILO and the
    nanorulers' TIRF (data/dyes/light_path.json, data/specimens.json), so the dropdown matches what a preset sets."""
    presets = json.loads((ROOT / "data" / "dyes" / "light_path.json").read_text(encoding="utf-8"))["presets"]
    specimens = json.loads((ROOT / "data" / "specimens.json").read_text(encoding="utf-8"))["specimens"]
    hilo = sorted({q["bfpPosition"] for q in presets if "bfpPosition" in q})
    tirf = sorted({sp["illumination"]["bfpPosition"] for sp in specimens if "illumination" in sp})
    if len(hilo) != 1 or len(tirf) != 1:
        sys.exit(f"gen_mm_configs: expected one HILO and one TIRF BFP position in the data, got {hilo} / {tirf}")
    return [("Epi", 0.0), ("HILO", hilo[0]), ("TIRF", tirf[0])]


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
        # Objective.ZernikePreset (Basic): the objective's aberrations, and the startup's
        "aberrations": list(core.getAllowedPropertyValues("Objective", "ZernikePreset")),
        "defaultAberrations": core.getProperty("Objective", "ZernikePreset"),
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
    w("# The simulated microscope (spec/MM_DEVICES.md, docs/micro-manager.md). Groups: Channel (the label mode and its")
    w("# typical dye; lasers and filter cube follow), Objective, Aberrations (the objective's Zernike set), Camera")
    w("# (sensor + its relay), Quality, Drift, Specimen, Illumination (epi / HILO / TIRF: the laser spot in the back")
    w("# focal plane)%s," % ("" if tier == "Basic" else ", BfpPosition (the same as a field)"))
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
    groups.append(("Aberrations", [(a, [("Objective", "ZernikePreset", a)]) for a in info["aberrations"]]))
    groups.append(("Camera", [(c, [("Camera", "CameraPreset", c), ("EmissionPath", "EmissionMagnification",
                                                                     num(CAMERA_RELAY[c]))]) for c in cameras]))
    groups.append(("Quality", [(q, [("Renderer", "Quality", q)]) for q in QUALITY if q in info["quality"]]))
    groups.append(("Drift", [(d, [("SampleHolder", "DriftPreset", d)]) for d in info["drift"]]))
    groups.append(("Specimen", [(s, [("SampleHolder", "Label", s)]) for s in info["specimens"]]))
    # Not in the Startup: the startup channel's light preset sets it (DNA-PAINT: HILO).
    illum = illumination_presets()
    groups.append(("Illumination", [(n, [("Lasers", "BfpPosition", num(u))]) for n, u in illum]))
    if tier != "Basic":   # (the field: Advanced and Expert; Basic picks Epi / HILO / TIRF)
        groups.append(("BfpPosition", [("NewPreset", [("Lasers", "BfpPosition", num(dict(illum)["HILO"]))])]))
    # One property, one preset ('NewPreset', as Studio's group editor names it): Studio shows the power as a field.
    for nm in LASER_GROUP_NM:
        groups.append((laser_group(nm), [("NewPreset", [("Lasers", laser_prop(nm), num(info["lasers"][nm]))])]))

    startup = [("Core", "ChannelGroup", "Channel")]
    if tier != "Basic":
        startup += [(MULTI, "Physical Shutter 1", "Lasers"), (MULTI, "Physical Shutter 2", "TransmittedLamp")]
    first = dict(groups)
    startup += dict(first["Channel"])[f"{typical[STARTUP_MODE]['dye']} {STARTUP_MODE}"]
    startup += dict(first["Objective"])[preset_name(info["defaultObjective"])]
    startup += dict(first["Aberrations"])[info["defaultAberrations"]]
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


LIGHT = {"Lasers": "epi", "TransmittedLamp": "trans", MULTI: "both"}


# Engine options that are the stage pose, the acquisition or the shutters' state, not a configuration's.
NOT_CONFIG = {"x", "y", "z", "seed", "frames", "exposure-ms", "start-sec", "light-epi", "light-trans", "modality",
              "light-preset", "pixel-nm"}
# The pixel size is derived (the camera's sensor pixel / (objective x emission magnification)): the presets carry these
# rows' values (registry keys -> names in the viewer), the viewer works the pixel size out of the current ones.
OPTICS = {"objective-mag": "objectiveMag", "sensor-pixel-um": "sensorUm", "emission-mag": "emissionMag"}
ZERN = re.compile(r"^zern\.\d+$")   # the spec's Zernike terms (WriteScopeSpecTo: the coefficients over the None preset)
# Options a preset changes only as a side effect the viewer makes itself: mounting the bead sample sets its bead
# density and aggregation when none is set and moves OnTop beads to the coverslip (MountSpecimen; the viewer's Sample
# select does the same), which are no part of the preset.
MOUNT_EFFECTS = {"Specimen": ("fid-density", "fid-aggregation-pct", "fid-height")}


def viewer_zernike_presets():
    """The viewer's Zernike presets in their select's order (web/index.html ZERNIKE_PRESETS = the engine's
    psf-zernike-preset indices): the Aberrations group (Objective.ZernikePreset) is that select in the viewer."""
    html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
    m = re.search(r"const ZERNIKE_PRESETS = \[(.*?)\]", html, re.S)
    if not m:
        sys.exit("gen_mm_configs: no ZERNIKE_PRESETS in web/index.html")
    return re.findall(r"'([^']+)'", m.group(1))


TIER_ORDER = {"Basic": 0, "Advanced": 1, "Expert": 2}


def option_tiers(core, spec_of):
    """The tier of every engine option a property drives: each settable registry row (Basic..Expert) is set to
    another value once (a choice, or a limit / the middle of its range), the options whose value moves in the engine
    spec are the row's, and each option takes the tier of the most specific row that moves it (the fewest options:
    CameraPreset moves a dozen, Camera.ReadNoiseElectrons one). -> {"options": {option: {"tier", "prop"}},
    "props": {"Device.Name": tier}} (every row, for a viewer control whose help names the property)"""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, "registry.json")
        core.setProperty("Renderer", "Test_WriteRegistryTo", f)
        rows = json.loads(Path(f).read_text(encoding="utf-8"))
    best = {}
    for r in rows:
        if r["tier"] not in TIER_ORDER or r.get("readOnly") or not core.hasProperty(r["device"], r["name"]):
            continue
        dev, name = r["device"], r["name"]
        cur = core.getProperty(dev, name)
        if r.get("choices"):
            alts = [c for c in r["choices"] if c != cur]
        elif r.get("lo") is not None and r["kind"] in ("float", "integer"):
            lo, hi = float(r["lo"]), float(r["hi"])
            alts = [v for v in (hi, lo, (lo + hi) / 2) if abs(v - float(cur)) > 1e-9 * max(1, abs(v))]
            alts = [str(int(round(v))) if r["kind"] == "integer" else repr(v) for v in alts]
        else:
            continue
        if not alts:
            continue
        try:
            before = spec_of(core)
            core.setProperty(dev, name, alts[0])
            after = spec_of(core)
            core.setProperty(dev, name, cur)
        except RuntimeError:
            continue
        moved = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        for k in moved:
            cand = (len(moved), TIER_ORDER[r["tier"]], dev + "." + name, r["tier"])
            if k not in best or cand < best[k]:
                best[k] = cand
    return {"options": {k: {"tier": v[3], "prop": v[2]} for k, v in sorted(best.items())},
            "props": {r["device"] + "." + r["name"]: r["tier"]
                      for r in sorted(rows, key=lambda r: (r["device"], r["name"])) if r["tier"] in TIER_ORDER}}


def viewer_presets(cfg_paths):
    """Per tier: the config groups of its file with, per preset, the engine options it sets (the engine spec the adapter
    renders from, Renderer.WriteScopeSpecTo, after the Startup preset and then this one: the options that differ
    between the group's presets or from a fresh hub's) and the light (the Core-Shutter it picks); a group of one
    numeric property with one preset is a slider with the registry row's limits. A preset that changes the pixel size
    carries its optics (OPTICS); "optics" per tier = the Startup's."""
    import tempfile
    from pymmcore_plus import CMMCorePlus

    os.environ["ISC_TEST"] = "1"   # the registry dump is a Test row; the configurations above never see Test rows
    # No block cache: hubs made and freed in quick succession, two at a time, have aborted the process in the cache
    # (2026-10-14; the specs need no cells).
    cache_env = os.environ.get("ISC_CACHE")
    os.environ["ISC_CACHE"] = "0"
    out, tiers = {}, {}
    with tempfile.TemporaryDirectory() as tmp:
        def spec(core, keep_all=False):
            f = os.path.join(tmp, "spec.txt")
            core.setProperty("Renderer", "WriteScopeSpecTo", f)
            kv = (line.split("=", 1) for line in Path(f).read_text(encoding="utf-8").splitlines() if "=" in line)
            return {k: float(v) for k, v in kv if keep_all or k not in NOT_CONFIG}

        def registry(core):
            f = os.path.join(tmp, "registry.json")
            core.setProperty("Renderer", "Test_WriteRegistryTo", f)
            return {(r["device"], r["name"]): r for r in json.loads(Path(f).read_text(encoding="utf-8"))}

        def load(text):
            f = Path(tmp) / "config.cfg"
            f.write_text(text, encoding="utf-8", newline="\n")
            core = CMMCorePlus()
            core.setDeviceAdapterSearchPaths(search_paths())
            core.loadSystemConfiguration(str(f))
            return core

        for tier, path in cfg_paths.items():
            # at Detail Expert (the spec row is Expert; rows above a tier keep their defaults, so nothing else moves)
            text = re.sub(r"(?m)^Property,Hub,Detail,\w+$", "Property,Hub,Detail,Expert", path.read_text(encoding="utf-8"))
            # a fresh hub: the file without its Startup settings (the Core's own kept)
            bare = load("".join(l for l in text.splitlines(True)
                                if not l.startswith("ConfigGroup,System,Startup,") or
                                l.startswith("ConfigGroup,System,Startup,Core,")))
            fresh = spec(bare)
            bare.unloadAllDevices()
            core = load(text)
            rows = registry(core)

            def optics(rs):
                return {name: float(r["value"]) for r in rs.values() for key, name in OPTICS.items()
                        if r.get("key") == key}
            start_optics = optics(rows)
            groups = []
            for g in core.getAvailableConfigGroups():
                if g == "System":
                    continue
                names = list(core.getAvailableConfigs(g))
                settings = {n: [tuple(x)[:3] for x in core.getConfigData(g, n)] for n in names}
                props = {(d, p) for n in names for d, p, _ in settings[n]}
                only = rows.get(next(iter(props))) if len(props) == 1 else None
                if len(names) == 1 and only and only.get("lo") is not None:
                    groups.append({"name": g, "slider": {"key": only["key"], "lo": float(only["lo"]),
                                                         "hi": float(only["hi"]), "value": float(only["value"])}})
                    continue
                states, opt = {}, {}
                for n in names:   # each on a fresh hub at Startup (a preset can leave a value it does not set behind)
                    one = load(text)
                    one.setConfig(g, n)
                    one.waitForSystem()
                    states[n] = spec(one)
                    opt[n] = optics(registry(one))
                    one.unloadAllDevices()
                # the options of the rows the presets set (a label's dye: -1 = Typical whatever the mode) and those
                # that differ between the presets or from a fresh hub's
                own = {rows[p]["key"] for p in props if p in rows and rows[p]["key"] in fresh}
                keys = sorted(own | {k for n in names for k, v in states[n].items()
                                     if v != fresh.get(k) or v != states[names[0]].get(k)})
                keys = [k for k in keys if k not in MOUNT_EFFECTS.get(g, ())]
                presets = []
                for n in names:
                    # an option the spec leaves out is at the engine's default (null; the specimen when it is 0)
                    pr = {"name": n, "set": {k: states[n].get(k, fresh.get(k)) for k in keys}}
                    # an Objective.ZernikePreset: the viewer's preset select, not the coefficients one by one
                    zp = [v for d, p, v in settings[n] if (d, p) == ("Objective", "ZernikePreset")]
                    if zp:
                        zernike = viewer_zernike_presets()
                        if zp[0] not in zernike:
                            sys.exit(f"gen_mm_configs: Zernike preset {zp[0]} is not one of the viewer's")
                        pr["set"] = {k: v for k, v in pr["set"].items() if not ZERN.match(k)}
                        pr["set"]["psf-zernike-preset"] = float(zernike.index(zp[0]))
                        pr["set"] = dict(sorted(pr["set"].items()))
                    shutter = [v for d, p, v in settings[n] if d == "Core" and p == "Shutter"]
                    if shutter:
                        pr["light"] = LIGHT.get(shutter[0], "epi")
                    moved = {k for m in names for k, v in opt[m].items() if v != start_optics[k] or v != opt[names[0]][k]}
                    if moved:
                        pr["optics"] = {k: opt[n][k] for k in sorted(moved)}
                    presets.append(pr)
                groups.append({"name": g, "presets": presets})
            core.setConfig("System", "Startup")
            core.waitForSystem()
            startup = {g: core.getCurrentConfig(g) for g in core.getAvailableConfigGroups() if g != "System"}
            out[tier] = {"groups": groups, "startup": startup, "optics": start_optics}
            if tier == "Expert":   # every row exists at Expert
                tiers = option_tiers(core, lambda c: {k: v for k, v in spec(c, True).items()
                                                      if k not in ("x", "y", "z", "seed", "frames")})
            core.unloadAllDevices()
    os.environ["ISC_TEST"] = "0"
    if cache_env is None:
        os.environ.pop("ISC_CACHE", None)
    else:
        os.environ["ISC_CACHE"] = cache_env
    return ("// The Micro-Manager configurations' groups for the viewer's live panel (web/live/mm_config_ui.js): generated\n"
            "// by tools/gen_mm_configs.py with adapter/inSiliScope/config/inSiliScope_{Basic,Advanced,Expert}.cfg, from the\n"
            "// engine spec the adapter renders each preset with -- do not edit.\n"
            "self.ISC_MM_CONFIG = " + json.dumps(out, indent=1) + ";\n"
            "// Each engine option's Micro-Manager tier (Basic / Advanced / Expert) and the property that sets it, and\n"
            "// each property's tier (a control whose help names the property): the viewer's settings levels follow them\n"
            "// (a control with neither keeps the viewer's own level).\n"
            "self.ISC_MM_TIERS = " + json.dumps(tiers, indent=0) + ";\n")


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
    import tempfile
    tmpdir = tempfile.TemporaryDirectory()
    cfg_paths = {}
    for tier in TIERS:
        text = build(tier, info, typical, cameras)
        path = OUT / f"inSiliScope_{tier}.cfg"
        cfg_paths[tier] = Path(tmpdir.name) / path.name   # the viewer's copy is made from the new text
        cfg_paths[tier].write_text(text, encoding="utf-8", newline="\n")
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {path.relative_to(ROOT)}")
    viewer = viewer_presets(cfg_paths)
    tmpdir.cleanup()
    if check:
        if not VIEWER_OUT.exists() or VIEWER_OUT.read_text(encoding="utf-8") != viewer:
            stale.append(VIEWER_OUT.name)
    else:
        VIEWER_OUT.write_text(viewer, encoding="utf-8", newline="\n")
        print(f"wrote {VIEWER_OUT.relative_to(ROOT)}")
    if stale:
        sys.exit(f"out of date (run tools/gen_mm_configs.py): {', '.join(stale)}")
    if check:
        print("MM configs up to date")


if __name__ == "__main__":
    main()
