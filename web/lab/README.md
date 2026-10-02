# Lab: iterate in JS, port to C++ at merge

Trying an idea (microtubule wiggliness, a new structure, cell shapes) must not cost a C++ port, a WASM build and
a golden re-freeze per try. So work happens in two phases.

## 1. Iterate (feature branch, JS only, seconds per try)

```
node web/lab/serve.mjs            # http://localhost:8123/web/lab/   (--base <git ref>, default origin/main)
cmake --build --preset lab         # the same, and opens it in the browser (after `cmake --preset msvc`)
```

- Edit `web/prototype/index.html` / `microtubules.js` (the generator is the truth for geometry). Saving reloads
  the working pane in place (view and tweaks kept); editing `web/lab/*` reloads the page.
- **A/B:** left = working tree, right = the same prototype at `--base` (served via `git show`), same seed, cell and
  panel tweaks (untick "apply panel tweaks to baseline" to compare against main's defaults). Flip mode: space toggles.
- **Panel:** built from the prototype's own `<input>`s, so a new slider shows up without lab changes. Orange = you
  changed it; the grey/orange column is main's default where it differs, `new` = not in main.
- **Views:** top view (MTs coloured by z, nucleus, red FOV square: click to move), x-z side view, shape metrics
  (curvature, tortuosity, turn radius, z slope; resampled at 0.1 um so independent of `mtStepLen`) with a curvature
  histogram, and the **imaging** row: a movie of the FOV (frame slider / play, mean projection, blink map = ideal
  localisations) rendered by the **JS imaging reference** (`web/prototype/scope/`, the C++ pipeline mirrored: dyes,
  blink kinetics, Gibson-Lanni+Zernike PSF, splat, camera noise, WideField, BrightField), next to the same movie from main's C++
  (its WASM module). With nothing changed the line above reads "100% identical pixels"; it drops as soon as your
  imaging change does something. Imaging options come from the C++ option table (panel groups; "lab" = a lab
  default for speed); "copy as cli command" gives the matching `insiliscope_cli` call.
- Iterate on photophysics, PSF, camera, WideField or BrightField by editing `web/prototype/scope/*.js` (the README there maps each
  file to its C++ twin); saves hot-reload like prototype edits (~5 s for a 64 px, 20-frame movie).
- Headless, ~15 s: `node web/lab/check.mjs [--cells 3] [--set mtWobbleTurn=1.2]`: prototype loads, no NaN geometry,
  cells independent of query history, metrics vs the baseline, an SR, a WideField and a BrightField movie of the JS
  imaging reference run. Also a CI job (`lab-check`).
- URL hash = full state (seed, cell, tweaks, preview); "copy link" shares a view.

Do **not** in this phase: touch `core/`, run cmake/ctest, re-freeze `spec/golden`, rebuild the viewer module, or
update docs. Instead keep `PORT_PENDING.md` at the repo root (its presence = iteration mode; CI's `port-gate` fails
and skips the C++/WASM/golden jobs, so the branch cannot merge):

```markdown
# Port pending
- What changed in the prototype (functions, hash channels, defaults, new params) and why.
- Lab link(s) showing the intended look.
- Anything the C++ side needs beyond a 1:1 port (new ABI field, adapter property, cli option).
```

## 2. Port (the PR to main, one go)

1. Port the prototype diff (`git diff origin/main -- web/prototype`): geometry to `core/` bit-exactly (CLAUDE.md core
   rules: `isc::jsm` math, JS operand order, types), imaging (`web/prototype/scope/`) to the C++ files its README names;
   then speed work.
2. `bash tools/port_check.sh` (`ISC_NATIVE=msvc` on Windows): re-freezes `spec/golden` (Node 24), lab check, native
   build + ctest, WASM build + ctest + viewer module + webSMLM block (if emsdk is present), parity report, and
   `tests/parity/scope_parity.mjs` (JS imaging == the rebuilt C++: SR 100% identical, WideField >= 99.9%, BrightField >= 99.5%).
3. Update `spec/PORT.md`, `spec/ALGORITHM.md`, `docs/`, adapter/cli/viewer options, gallery; delete
   `PORT_PENDING.md`; merge when CI is green.

Files: `serve.mjs` (dev server: static + `/baseline/` + reload events), `lab.js`/`index.html` (page),
`lab_worker.js` (one prototype instance per worker, or a ref's WASM for the C++ movie), `field.js` (packing window,
world-space MTs), `metrics.js`, `check.mjs`. Imaging: `web/prototype/scope/`; C++ side for comparisons:
`tests/parity/wasm_scope.mjs`. The prototype is loaded by `tests/parity/load_prototype.mjs`, the same
loader the golden reference uses.
