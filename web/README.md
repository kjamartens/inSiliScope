# Cell-field viewer

Pan/zoom/tilt viewer for the infinite cell field. Open `index.html` directly in a browser -- no build
step, no server.

The generator is the **insiliscope core** (`core/`, C++) compiled to WASM: `insiliscope_module.js` holds
the Emscripten module as source text (WASM inlined as base64; generated, do not edit). The same module
carries the adapter's render code (`isc_scope_movie`): the "blink movie" panel renders a few camera
frames of the dyes under the view centre with the fluorophore/camera settings given there, plays them,
and saves them as a 16-bit TIFF (identical to `cli/insiliscope_cli` with the same settings). Web Workers
evaluate it and answer cell/asset/dye requests through the C ABI; the page itself keeps only UI,
Canvas 2D and WebGL. Cells are packed on fixed 8x8-chunk blocks (as in the adapter), so a view shows
exactly what the Micro-Manager adapter images. `?nw` runs the core on the main thread, `?2d` forces the
Canvas 2D fallback (both for testing).

Rebuild the module after a core change:

    source ~/emsdk/emsdk_env.sh
    cmake --preset wasm && cmake --build --preset wasm
    node tools/embed_web_module.mjs          # --check: fail if web/insiliscope_module.js is stale

`prototype/` is the original JS prototype (`index.html` generator half + `microtubules.js`): the
**reference implementation** the core is ported from and tested against (`tests/parity`,
`spec/golden`), so its generator changes only deliberately (see [spec/PORT.md](../spec/PORT.md)).
Algorithm notes: [spec/ALGORITHM.md](../spec/ALGORITHM.md).

Tools (`cd tools && npm install` once, or a global Playwright):
- `node tools/bench_pan.mjs [page]` -- pan benchmark (300 frames, headless Chromium, software GL).
- `node tools/check_cellfield_microtubules.mjs` -- the prototype's microtubule regression check.
