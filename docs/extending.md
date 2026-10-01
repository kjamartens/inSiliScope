# Extending inSiliScope

The design rule: **every piece of the world is a pure function of `(seed, address, channel)`**, and consumers see only the
C ABI. Keep that and a new feature works in the Micro-Manager adapter, the viewer and webSMLM at once.

## Layers

| Layer | Directory | Rule |
|---|---|---|
| World model (cells, structures, dyes, schedules) | `core/` | C++17, no MMDevice or GPU/OS dependencies, `extern "C"` ABI only |
| Rendering (PSF, noise, widefield) | `adapter/inSiliScope/Simulation/` | no MM includes; also built by the CLI and the viewer's WASM target |
| Device adapter | `adapter/inSiliScope/` | MSBuild, MM properties |
| Viewer | `web/` | generated module + UI |
| webSMLM block | `tools/make_cellfield_block.mjs` | generated |

## Recipe: a new structure or cell type

1. Pick a fresh hash **channel** number; never reuse one (a reused channel correlates two decisions).
2. Generate it per cell in the cell's local frame from `Pcg4d(seed ^ SALT, cx, cy, channel)`. Decide everything cheap
   (exists? labelled?) from hashes before building geometry.
3. Generate in *blocks* with a stable address so a window query touches only the blocks it needs; cache them in an LRU.
4. Expose the sites through the existing dye/event queries, so both modalities pick it up.
5. Add `world_checks` cases: order independence, tiling, 8 threads = 1 thread.
6. Document the model on a page under `docs/physics/` (equations, parameters with units and defaults, sources, limits).
7. Show it in the overview: add its geometry to `ScopeGeometryJson` (`Simulation/ScopeMovie.cpp`, `insiliscope_cli
   --geometry-json`) and one draw function to `STRUCTURE_LAYERS` in `tools/build_overview.py`.

## Recipe: a new photophysics preset

dSTORM/PALM/DNA-PAINT presets are combinations of the labelling fractions, activation rate and the blink parameters
(`Kinetics` on the `World`). Add a named preset in the consumers (cli option, viewer, MM property) and a gallery entry.

## Recipe: a new MM property

Give it one of the six group prefixes (`General_`, `SimType_`, `FluoParam_`, `CamParam_`, `PSFParam_`, `Background_`), update the
prefix list in `CLAUDE.md`, and add the same option to `ScopeMovieOptions()` so the CLI and viewer reach it.

## Recipe: a new modality

Follow WideField or BrightField: a render path in `Simulation/`, a modality value in `General_ImagingModality`, a
CLI/viewer option, an entry in `gallery/manifest.json`, a CI check, and a section in the physics docs. A modality that
needs new geometry gets it from a core query (BrightField: `isc_optical_volume_in_window`, pure geometry; the optics
live in the renderer). A new structure should also say what it does to brightfield (its refractive index) and be added
to the optical volume, so brightfield never shows structures the world does not simulate.

## Recipe: a gallery entry

Add an entry to `gallery/manifest.json` (options are `insiliscope_cli` options without the leading `--`). The release
workflow renders it. The 2x2 overview at the top of the gallery and on the Home page comes from the manifest's
`overview` block (`tools/build_overview.py`; `--suggest` proposes a spot); `build_gallery.py --only overview` renders
just that.

## Bit-exactness and `world_version`

The existing content is bit-exact with the JS prototype and frozen by golden vectors. Content with no JS counterpart
(anything new) cannot be, so its reference is the C++ itself:

- add golden vectors generated from the C++ and commit them; a change to them is a deliberate, reviewed act;
- changing existing output (a default, a fix that moves cells) is a **breaking change for seeds**: record it in `CHANGELOG.md`;
- the plan is a `world_version` in the ABI, stamped in every output, so a seed can be reproduced on the version that made it.

Changes to the prototype itself follow the rule in `CLAUDE.md`: update `spec/` and re-freeze `spec/golden` in the same commit.

## Checklist for a pull request

- ctest passes natively and under WASM (see `CLAUDE.md`);
- `node tools/embed_web_module.mjs --check` is clean (regenerate after a core change);
- adapter output unchanged by a refactor: `tools/adapter_pixel_hash.py` before and after;
- docs and the gallery updated.
