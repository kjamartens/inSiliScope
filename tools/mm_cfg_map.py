# SPDX-License-Identifier: BSD-3-Clause
"""Which Micro-Manager property sets each engine option, and how its values map: the table the viewer's ".cfg from the
viewer" export writes a configuration with (web/live/mm_cfg.js; tools/gen_mm_configs.py puts it in
web/live/mm_config.js as self.ISC_MM_CFG).

Learnt from the adapter, not written by hand: every settable row of the registry (Renderer.Test_WriteRegistryTo, at
Detail Expert) is set to other values on a fresh hub and the engine spec it renders with (Renderer.WriteScopeSpecTo)
is read back. A number row whose value lands as is in an option maps that option 1:1 ("num"); a choice row maps an
option whose value differs for every choice ("choice": option value -> the choice's name). A target's Label is
nudged in every mode (its choices follow the mode). Each option goes to the row that moves the fewest options (the
member, not a combination: Camera.GainElectronsPerADU, not Camera.CameraPreset); a configuration sets the rows in the
order of how many options they move, most first, so a combination (a camera preset, a label, a pattern) comes before
the members it sets.

Options with a writer of their own (SPECIAL) and the viewer's options that are not configuration (VIEWER_ONLY, with
the reason) are listed here too: tests/web/viewer_mm_cfg.mjs fails on a viewer option that is in none of them, so a
new viewer setting needs a Micro-Manager property (spec/MM_DEVICES.md) or a line in VIEWER_ONLY.
"""

import re

# The viewer's options that are no configuration, and why (the .cfg leaves them; the round trip skips them).
VIEWER_ONLY = {
    "x": "the stage position (XYStage), not a configuration",
    "y": "the stage position (XYStage), not a configuration",
    "z": "the focus (ZStage), not a configuration",
    "frames": "an acquisition's length",
    "start-sec": "a movie's start time (Micro-Manager keeps the sample's own clock)",
    "world-seed": "the cell field's seed, derived from the seed (Hub.RandomSeed) as in Micro-Manager",
    "psf-kernel-half-width-nm": "the viewer's 3 um kernel (WASM memory); Micro-Manager keeps Objective.PsfKernelHalfWidthNm (the user's choice, 2026-11-19)",
    "bfp-map-px": "the raster of the Microscope view's BFP readout, never a frame",
    "p.showCytoContours": "the world view's drawing",
    "p.cytoContourStep": "the world view's drawing",
    "p.mtLineWidth": "the world view's drawing",
    "prepare": "a PSF preload request",
}

# Options the writer handles itself (web/live/mm_cfg.js): the Zernike set, the beam matrix, the BFP maps' text, the
# derived pixel size (the objective's magnification, the sensor pixel, the emission magnification), the seed (pre-init),
# the light (the Core-Shutter), the exposure and the camera size (Camera properties outside the registry).
SPECIAL = {
    "psf-zernike-preset": "Objective.ZernikePreset",
    "zern.*": "Objective.ZernikeCoefficients",
    "beam.*": "Lasers.Beams",
    "sim-pair-phase.*": "Lasers.PatternPhaseMatrix",
    "bfp-maps": "Lasers.BfpMaps",
    "pixel-nm": "Objective.Magnification x EmissionPath.EmissionMagnification / Camera.SensorPixelUm",
    "seed": "Hub.RandomSeed (pre-init)",
    "modality": "the Core-Shutter",
    "light-epi": "the Core-Shutter",
    "light-trans": "the Core-Shutter",
    "exposure-ms": "Camera.Exposure",
    "size": "Camera.SensorWidthPx / SensorHeightPx",
    "height": "Camera.SensorHeightPx",
}

# The viewer's names of options Micro-Manager writes under another (the world's params as the engine's own options).
ALIASES = {
    "p.chunkSize": "chunk-um", "p.density": "occupancy", "p.cellDiamMin": "cell-diam-min-um",
    "p.cellDiamMax": "cell-diam-max-um", "p.mtDensity": "mt-density", "p.npcDensity": "npc-density",
    "p.laminDensity": "lamin-density", "p.enablePacking": "packing", "p.specimen": "specimen",
    "p.origamiDesign": "origami-design", "p.origamiSpacingNm": "origami-spacing-nm",
    "p.origamiDensity": "origami-density", "p.origamiMinSpacingNm": "origami-min-spacing-nm",
    "p.origamiBend": "origami-bend", "p.origamiLpNm": "origami-lp-nm",
    "p.origamiStrandsPerSpot": "origami-strands-per-spot", "p.origamiAttachNm": "origami-attach-nm",
    "p.origamiLinkerNm": "origami-linker-nm", "p.origamiRoll": "origami-roll", "p.origamiRollDeg": "origami-roll-deg",
    "p.origamiHeights": "origami-heights",
}

STATE_DEVICES = ("Objective", "FilterCube", "ExcitationFilter", "Dichroic", "EmissionFilter", "SampleHolder")
NOT_ROWS = {"x", "y", "z", "frames", "seed", "start-sec", "frame0"}


def jsnum(v):
    """A number as JavaScript's String(v) writes it (the writer looks a choice up by it): 3, -1, 0.95."""
    v = float(v)
    return str(int(v)) if v.is_integer() and abs(v) < 1e21 else repr(v)


def _close(a, b):
    return a is not None and b is not None and abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def build(load_core, spec_of, registry_of):
    """-> {"rows": [{dev, prop, tier, kind, n, opts: {option: map | None}}], ...}. load_core(): a fresh hub at Detail
    Expert with every device (ISC_TEST=1); spec_of(core): the engine spec {key: float}; registry_of(core): the rows."""
    rows_out = []
    core = load_core()
    reg = registry_of(core)
    fresh = {k: jsnum(v) for k, v in spec_of(core).items() if k not in NOT_ROWS}
    core.unloadAllDevices()
    modes = None
    by_device = {}
    for r in reg:
        if r["tier"] not in ("Basic", "Advanced", "Expert") or r.get("readOnly"):
            continue
        if r["name"].startswith(("Write", "Test_")) or (r["kind"] == "text" and not r.get("choices")
                                                         and not r.get("key", "").endswith(".fields")):
            continue   # (actions; the text rows the writer handles itself: SPECIAL)
        by_device.setdefault(r["device"], []).append(r)
    passes = [(d, False) for d in by_device] + [(d, True) for d in STATE_DEVICES]
    for dev, state in passes:
        core = load_core()
        if modes is None:
            modes = list(core.getAllowedPropertyValues("CellField", "Microtubules_Mode"))
        if state:   # a state device's position (its Label)
            items = [{"device": dev, "name": "Label", "tier": "Advanced" if dev in ("ExcitationFilter", "Dichroic",
                      "EmissionFilter") else "Basic", "kind": "text",
                      "choices": list(core.getStateLabels(dev))}]
        else:
            items = by_device[dev]
        for r in items:
            name = r["name"]
            if not core.hasProperty(dev, name):
                continue
            cur = core.getProperty(dev, name)
            base = spec_of(core)
            moved, maps, sets = set(), {}, {}
            if r.get("key", "").endswith(".fields"):
                # a dye field: the edit <prefix>-dye.<field> / dye<N>.<field> (in the spec only when it differs from
                # the dye's own value, so by name: the row's suffix in kebab case)
                field = re.sub(r"(?<=[a-z])(?=[A-Z0-9])", "-", name.split("_Dye", 1)[-1] if "_Dye" in name
                               else name.split("_", 1)[1]).lower()
                rows_out.append({"dev": dev, "prop": name, "tier": r["tier"], "kind": "num", "n": 0,
                                 "key": r["key"], "opts": {r["key"][:-len("fields")] + field: None}})
                continue
            if r.get("choices"):
                # a target's Label in every mode (its list is the mode's)
                mode_prop = name[:-len("_Label")] + "_Mode" if name.endswith("_Label") else None
                in_modes = modes if mode_prop and core.hasProperty(dev, mode_prop) else [None]
                cur_mode = core.getProperty(dev, mode_prop) if in_modes != [None] else None
                seen = {}   # option -> {value: choice}
                per_choice = {}   # choice -> option -> {values}
                clash = set()
                for mode in in_modes:
                    if mode is not None:
                        core.setProperty(dev, mode_prop, mode)
                    for c in core.getAllowedPropertyValues(dev, name):
                        try:
                            core.setProperty(dev, name, c)
                            core.waitForSystem()
                        except RuntimeError:
                            continue
                        s = spec_of(core)
                        moved |= {k for k in set(s) | set(base) if s.get(k) != base.get(k)}
                        for k in set(s) | set(base):
                            v = s.get(k)
                            m = seen.setdefault(k, {})
                            if m.get(v, c) != c:
                                clash.add(k)
                            m[v] = c
                            per_choice.setdefault(c, {}).setdefault(k, set()).add(v)
                if mode is not None:
                    core.setProperty(dev, mode_prop, cur_mode)
                try:
                    core.setProperty(dev, name, cur)
                except RuntimeError:
                    pass
                for k in moved - NOT_ROWS - clash:
                    if k in seen and len(set(seen[k].values())) >= 2:   # (None: the option left out = the engine's default)
                        maps[k] = {("null" if v is None else jsnum(v)): c for v, c in sorted(
                            seen[k].items(), key=lambda kv: (kv[0] is None, kv[0] or 0))}
                kind = "choice"
                # what each choice sets (the options it moves, where they do not depend on the mode)
                sets = {c: {k: jsnum(next(iter(vs))) for k, vs in sorted(per.items())
                            if k in moved and k not in NOT_ROWS and len(vs) == 1 and None not in vs}
                        for c, per in per_choice.items()}
            elif r.get("lo") is not None and r["kind"] in ("float", "integer"):
                lo, hi = float(r["lo"]), float(r["hi"])
                vals = [lo + 0.37 * (hi - lo), lo + 0.61 * (hi - lo)]
                if r["kind"] == "integer":
                    vals = [float(round(v)) for v in vals]
                    if vals[0] == vals[1]:
                        vals = [lo, hi]
                ident = None
                for v in vals:
                    try:
                        core.setProperty(dev, name, str(int(v)) if r["kind"] == "integer" else repr(v))
                        core.waitForSystem()
                    except RuntimeError:
                        continue
                    got = float(core.getProperty(dev, name))
                    s = spec_of(core)
                    hit = {k for k in s if _close(s.get(k), got)} - NOT_ROWS
                    moved |= {k for k in set(s) | set(base) if s.get(k) != base.get(k)}
                    ident = hit if ident is None else ident & hit
                try:
                    core.setProperty(dev, name, cur)
                except RuntimeError:
                    pass
                for k in (ident or set()) & moved:
                    maps[k] = None
                kind = "num"
            else:
                continue
            moved -= NOT_ROWS
            if maps:
                rows_out.append({"dev": dev, "prop": name, "tier": r["tier"], "kind": kind, "n": len(moved),
                                 "key": r.get("key"), "opts": dict(sorted(maps.items())),
                                 **({"sets": sets} if kind == "choice" else {})})
        core.unloadAllDevices()
    return {"rows": rows_out, "fresh": fresh}


def table(built, zernike):
    """The writer's table: each option's row (the one that moves the fewest options; the row whose key is the option on
    a tie), the rows in the order a configuration sets them (a target's Mode, then its Label, then by the options a row
    moves, most first: a combination before its members), and the lists above."""
    rows = built["rows"]
    best = {}
    for i, r in enumerate(rows):
        for o in r["opts"]:
            c = (r["n"], 0 if r.get("key") == o else 1, i)
            if o not in best or c < best[o][0]:
                best[o] = (c, i)
    chosen = {}
    for o, (_, i) in best.items():
        chosen.setdefault(i, []).append(o)

    def rank(r):
        return (0 if r["prop"].endswith("_Mode") else 1 if r["prop"].endswith("_Label") else 2, -r["n"])
    order = sorted(chosen, key=lambda i: (rank(rows[i]), i))
    out = []
    for i in order:
        r = rows[i]
        row = {"dev": r["dev"], "prop": r["prop"], "tier": r["tier"], "kind": r["kind"],
               "opts": {o: r["opts"][o] for o in sorted(chosen[i])}}
        if r.get("sets") and r["n"] > 1:   # a combination: what its choices set (the writer's view of the state)
            row["sets"] = r["sets"]
        out.append(row)
    return {"rows": out, "fresh": built["fresh"], "special": SPECIAL, "viewerOnly": VIEWER_ONLY, "aliases": ALIASES,
            "zernike": zernike}
