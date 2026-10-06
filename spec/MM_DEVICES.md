# Micro-Manager devices and property tiers

How the inSiliScope Micro-Manager adapter is divided into devices, which tier each property gets, and how to add new
inputs, targets, specimens, light sources and devices. **Follow it when you add or move anything in
`adapter/inSiliScope/`**; change this file in the same commit when a rule changes.

## 1. The model: a microscope, layer by layer

The adapter is one MM **hub** (`inSiliScope`) with **peripherals**. Each peripheral is a component a microscopist
recognises, and owns the settings of that component, nothing else:

```
specimen -> targets -> labels -> fluorophores -> excitation light -> objective -> emission path -> filters -> camera
            (SampleHolder mounts one specimen)                       (+ transmitted lamp)
Renderer: how the images are computed (numerics only)     Hub: session state (seed, Detail) and the shared settings
```

| Device | MM type | Owns | Must not own |
|---|---|---|---|
| `inSiliScope` (hub) | Hub | pre-init `Detail`, `RandomSeed`; the settings (`Registry/SceneState`), the stage state, cross-device couplings | anything a user tunes during a session |
| `Camera` | Camera | the sensor (noise chain, QE curve, sensor pixel, bit depth), exposure, binning, pre-init `FovSize`, acquisition | optics, specimen, render numerics |
| `XYStage`, `ZStage` | XYStage, Stage | commanded motion (position, speed, settle, limits, z sequences) | drift (that is the sample's) |
| `Objective` | State (turret) | the detection pupil: NA, immersion, magnification, aberrations (Zernike), the PSF kernel's extent | the emission magnification, the mask of a relay |
| `EmissionPath` | Magnifier | magnification between objective and camera (tube lens / relay); MM divides the pixel size by it | |
| `FilterCube` | State | a *combination*: one (excitation filter,) dichroic, emission filter choice | the filters' own data |
| `Dichroic`, `EmissionFilter` (Advanced) | State (wheels) | one filter each; their Custom edges | |
| `Lasers` | Shutter | excitation: line powers, the custom line, beam profile, geometry, the light preset; its shutter = the epi light | filters |
| `TransmittedLamp` | Shutter | transmitted light: intensity, condenser NA, wavelength; its shutter = the BrightField light | |
| `SampleHolder` | State | which specimen is mounted; what belongs to the sample whatever it is: drift, background, the medium of the PSF | a specimen's geometry |
| `CellField` | Generic | the cell field's geometry and optics (indices), its targets' labelling (which dye, how dense) | photophysics of a dye |
| `Fluorophores` | Generic | the dyes: label mode (from the label-model commit), photophysics readouts, custom dyes | which target is labelled |
| `Renderer` | Generic | numerics and machinery: `Quality`, PSF model/oversampling/interp, mean-field limits, BrightField sampling, GPU, caches, debugging | physics |

The imaging follows the light: the open shutters decide what a frame shows (epi light: fluorescence; lamp alone:
BrightField; none: a dark frame). There is no modality setting.

## 2. Where a new input goes

Answer in this order; **the first yes wins**:

1. Does it only change accuracy or speed, not the modelled physics (sampling, oversampling, caches, GPU, a numerical
   tolerance, a debug switch)? **Renderer** (usually Expert; one Basic knob: `Quality`).
2. Is it set once per session and needs a reload (seed, the tier)? **Hub, pre-init**.
3. Does it belong to the sample whatever the specimen is (drift, mounting medium, background autofluorescence,
   coverslip)? **SampleHolder**.
4. Is it the geometry of one specimen type? **That specimen's device** (e.g. `CellField`). Of one labelled structure
   (a target) in it? The target's rows, made by the per-target helper (section 4).
5. Is it a dye's photophysics or the labelling mode / buffer? **Fluorophores**.
6. Which dye sits on which target, and how densely? **The target's label rows** on its specimen.
7. Excitation light? **Lasers** (or a new epi light source, section 5).
8. Transmitted light? **TransmittedLamp**.
9. The detection pupil (NA, immersion, aberrations, kernel extent)? **Objective**.
10. Magnification after the objective? **EmissionPath**.
11. Spectral filtering? The **filter wheel** of that filter; the **FilterCube** combines them.
12. The sensor or the acquisition? **Camera**.
13. Commanded motion? **XYStage / ZStage**.

Still unsure: it probably needs a new device (section 3) or is not a user setting at all (a constant in the engine).

## 3. When to add a device

Add a device when **one** of these holds:

- it is a separate physical component a microscopist would control on its own (a wheel, a shutter, a turret, a light
  source, a focus lock);
- it needs a different MM device type to use MM features (config groups and channels: State; autoshutter: Shutter;
  pixel calibration: Magnifier; hardware sequencing: Stage/State);
- it is a new specimen type (one device per specimen, section 4).

**Do not** add a device to split a long property list, for a new target in an existing specimen, or for a new option
of an existing component. A new device needs, in one commit: its class (`Devices/`), an entry in
`isc::Peripherals()` (`InSiliScopeHub.cpp`: name, description, **device tier**), `CreateDevice` / `RegisterDevice`
(`InSiliScopeModule.cpp`), its registry rows, `inSiliScope.vcxproj(.filters)`, the config generator, the docs, and
`tools/isc_mm.py`'s device list.

## 4. Specimens and targets

- One **specimen** device per kind of sample (`CellField` now; a DNA-PAINT nanoruler, a bead slide, NPCs on a
  coverslip later). `SampleHolder` mounts exactly one; optional **overlays** (fiducial beads, autofluorescence) add on
  top. Specimens do not overlap in space: one is mounted at a time.
- A specimen has **targets**: its labelled structures (CellField: `Microtubules`; later NPCs, DNA, actin). Each target
  gets the same label rows (`<Target>_Dye` / `_Label`, `_LabelingPct`, `_ImagerNm`, `_Mode`, `_Orientation`, ...), so
  a new target is a registry entry, not new code. The photophysics is shared: every target's label goes through the
  same dye library and `MakeEffectiveDye` / `MakeLabelPhysics`, whatever the specimen.
- The geometry of a new specimen or target lives in `core/` behind the C ABI, so the viewer, the cli and webSMLM get
  it too. The adapter never generates geometry of its own.

## 5. Light sources

A light source is a Shutter device; its open state is part of every frame (`SetLight` on the hub). Today: `Lasers`
(epi: fluorescence) and `TransmittedLamp` (BrightField). A frame with both open sums them (from the shutter commit
on), with none open is dark. A new source (an epi LED, a second laser bank, a TIRF illuminator) is a new Shutter
device and an engine option `light-<name>`; never a modality switch.

## 6. Tiers

Every property and every device has a tier. The hub's pre-init `Detail` (Basic / Advanced / Expert) decides which
rows exist in a session; a row above it is never created and its setting keeps its default.

| Tier | Rule | Examples |
|---|---|---|
| **Basic** | What a microscopist changes at the instrument during a session to get the image they want. **At most 30 Basic properties over all devices** (`tools/test_insiliscope.py` checks it): adding one means justifying it or demoting another. | exposure, binning, FOV, camera preset, laser powers, light preset, beam profile, objective, filter cube, label, quality |
| **Advanced** | Physical parameters a user knows the meaning of and may tune to match their own setup. Read-only readouts. | noise values, NA, refractive indices, labelling density, drift, background, filter wheels, stage speed, GPU switch, disk cache |
| **Expert** | Numerics, caches, debugging, model-shape parameters, overrides of library data, custom dyes. | PSF model and oversampling, BrightField sampling, mean-field limits, nucleus shape, Zernike coefficients, Dye1-3 |
| **Test** | Only what automated checks need (deterministic stacks, hooks). Never shown in MM: created only when the process has `ISC_TEST=1`, whatever `Detail` says; named `Test_*`. Never something a user needs to reproduce a result. | `Test_AcqMode`, `Test_GenerateStack`, `Test_StackLength` |

- A readout is Advanced (Expert if it only helps debugging).
- Pre-init only when a change needs a reload (`RandomSeed`, `Detail`, `FovSize`).
- Device tiers follow the same rules: the hub's `DetectInstalledDevices` offers the devices at or below `Detail`
  (the filter wheels are Advanced: in Basic the `FilterCube` holds the filters).

## 7. Couplings

- A **combination or preset** (FilterCube, `Lasers.Preset`, `Camera.CameraPreset`, `Objective` positions,
  `Renderer.Quality`, MM config groups) sets its members. Setting a member by hand turns the combination to `Custom`
  (or `None` for a light preset); a position that is a combination of other settings is derived from them, not
  stored.
- Effects across devices go **only** through the hub: a row's setter changes `SceneState`, the coupling calls
  `InSiliScopeHub::Notify(key)` for every setting it changed, and each device refreshes its rows bound to that key.
  No device holds a pointer to another.
- What a change invalidates is the row's `invalidate` (None / Stack / Live / All).

## 8. The engine contract

- Every physical setting is an engine option of `ScopeSpec` (`Simulation/ScopeMovie`; JS twin
  `web/prototype/scope/scope_movie.js`; the viewer), so the cli, the viewer and webSMLM can render the same frame.
  `Registry/SceneSettings.cpp` turns the settings into the spec; `Renderer.WriteScopeSpecTo` writes it to a file.
- MM-only plumbing (shutters, turret positions, Test rows, caches) stays in the adapter.
- Specimens and targets go through the engine's registry (from the label-model commit), never through adapter code.

## 9. Checklists

**A new property:** one row in `Registry/PropertyTable.cpp` (device, name without group prefix, tier, binding to a
`SceneState` setting or engine option, limits/choices, invalidate, one-line help); the setting in `SceneState` and in
`BuildScopeSpec` if new; couplings in `Registry/Couplings.cpp`; a check in `tools/test_insiliscope.py` or
`tools/test_cellfield_stage.py`; the generated property reference and configs; docs; a gallery entry if it changes the
image (not for caches or preparation switches).

**A new target in a specimen:** the core geometry; its registry entry (target name, structure, option prefix,
typical labels per mode with references); nothing else in the adapter.

**A new specimen:** the core generator and ABI; a specimen device (section 3); its `SampleHolder` position; its
targets (above); the viewer's specimen choice comes from the same registry.

**A new light source:** a Shutter device (section 5), the engine option, a parity case, the configs.

**Naming:** the device is the group, so property names carry no group prefix (`NA`, not `PSFParam_PsfNa`);
per-target rows are `<Target>_<Name>`; sensor-specific rows are `sCMOS_` / `EMCCD_`; MM's standard keywords
(`Exposure`, `Binning`, `State`, `Label`, `Position`, ...) stay as they are.

## 10. Files

| File | What |
|---|---|
| `InSiliScopeHub.*` | the hub: Detail, RandomSeed, the device list (`Peripherals()`), notifications |
| `Registry/SceneState.*` | every setting (atomics; options and strings under a mutex) |
| `Registry/PropertyTable.*` | the property registry: one row per property |
| `Registry/RegistryDevice.h` | the base of every device: creates its rows for the session's tiers, the generic handler |
| `Registry/Couplings.cpp` | presets, dye loads, camera presets |
| `Registry/SceneSettings.*` | the settings as the engine's spec and structures |
| `Devices/Peripherals.*` | the turrets, wheels, shutters, magnifier and generic devices |
| `InSiliScopeCamera.*`, `SMLMImageGeneration.cpp` | the camera: acquisition, live loop, precomputed stack (Test) |
| `InSiliScopeXYStage.*`, `InSiliScopeZStage.*` | the stages |
| `tools/isc_mm.py` | loading the devices in pymmcore-plus (the test scripts) |
