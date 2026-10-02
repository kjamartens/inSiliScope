# Lab: iterate in JS, port to C++ at merge

Trying an idea (microtubule wiggliness, a new structure, cell shapes) must not cost a C++ port, a WASM build and
a golden re-freeze per try. So work happens in two phases.

## 1. Iterate (feature branch, JS only, seconds per try)

```
cmake --build --preset lab         # starts the dev server and opens lab.html (after `cmake --preset msvc` once)
node web/lab/serve.mjs [--open]    # the same without CMake (--base <git ref> for the A/B page, default origin/main)
```

### lab.html: the viewer on the JS reference (http://localhost:8123/web/lab.html)

`web/index.html` itself (same page, UI, drawing and movies: the server swaps only its WASM module for
`self.ISC_ENGINE_URL = 'lab/engine.js'`), with every answer computed by the JS: cells, outlines, meshes and
microtubules by the prototype (`web/prototype/index.html`, `microtubules.js`), dyes and SR / WideField / BrightField
movies by `web/prototype/scope/`. `web/lab/engine.js` is the glue: the viewer's `iscEngine` protocol (`pack`, `cell`,
`sites`, `movie`) on `World` and `renderScopeMovie`.

- With nothing changed it shows exactly what `index.html` shows (`node web/lab/engine_check.mjs`: cells, assets and
  dyes identical, SR and WideField movies identical, BrightField >= 99.5%).
- Edit the JS, save: the page reloads by itself and keeps the view and every control you changed (click the orange
  badge to forget them). Saves under `web/prototype`, `web/lab` and of `web/index.html` / `wf_gpu.js` all reload it.
- The viewer's control values are sent as they are; the prototype's own defaults only fill in what the viewer has no
  control for. A new parameter: add it to the prototype's generator and `params()`, and a control for it to
  `web/index.html` (that UI is what ships); a key only the viewer sends still reaches the generator.
- Slower than the WASM (no threads, no caches, no WebGPU): the first SR movie spends ~10 s on the PSF in JS.
- `?nw` runs the JS engine on the main thread, as in the viewer.

### The A/B page (http://localhost:8123/web/lab/)

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
   `tests/parity/scope_parity.mjs` (JS imaging == the rebuilt C++: SR 100% identical, WideField >= 99.9%, BrightField >= 99.5%),
   and `web/lab/engine_check.mjs` (lab.html == index.html on the rebuilt module).
3. Update `spec/PORT.md`, `spec/ALGORITHM.md`, `docs/`, adapter/cli/viewer options, gallery; delete
   `PORT_PENDING.md`; merge when CI is green.

Files: `serve.mjs` (dev server: static + `/web/lab.html` + `/baseline/` + reload events), `engine.js` (lab.html's JS
engine), `lab_html.js` (lab.html's badge and reload), `engine_check.mjs`, `lab.js`/`index.html` (page),
`lab_worker.js` (one prototype instance per worker, or a ref's WASM for the C++ movie), `field.js` (packing window,
world-space MTs), `metrics.js`, `check.mjs`. Imaging: `web/prototype/scope/`; C++ side for comparisons:
`tests/parity/wasm_scope.mjs`. The prototype is loaded by `tests/parity/load_prototype.mjs`, the same
loader the golden reference uses.
