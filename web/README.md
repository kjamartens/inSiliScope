# Cell-field viewer

Pan/zoom/tilt/turn viewer for the infinite cell field. Open `index.html` directly in a browser -- no build
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

WideField movies run their convolution on WebGPU when the browser has a hardware adapter
(`wf_gpu.js`, the kernels of `adapter/inSiliScope/Simulation/WidefieldGpu.wgsl`, embedded in
`insiliscope_module.js`); the movie's info line says "GPU: ..." or "CPU". A software adapter is not
used unless `?wfgpu=any` (tests); any GPU failure falls back to the CPU.

Rebuild the module after a core change:

    source ~/emsdk/emsdk_env.sh
    cmake --preset wasm && cmake --build --preset wasm
    node tools/embed_web_module.mjs          # --check: fail if web/insiliscope_module.js is stale

`scene/core.js` is the viewer's scene core (DOM-free, `tests/web/scene_core_check.mjs`): the camera (rotation about the
view centre, tilt, pivot height), the structure/layer registry (`structure.rep` ids: geometry layers of every
structure, data layers of structures with dyes), the themes and the packing of per-layer clip intervals into the
shaders' tables.

`anim/` is the Animation tab: `sequence.js` the sequence model and its pure evaluator (DOM-free,
`tests/web/anim_unit.mjs`), `editor.js` the editor, timeline, preview and export. `encode/` holds the export encoders
(`gif.js`, `mp4.js`, `webm.js`: DOM-free, `tests/web/encode_unit.mjs`; `video.js`: WebCodecs sinks with fallbacks).
`tests/web/viewer_anim_export.mjs` exports and plays back in a browser (`--channel=chrome` for H.264).

`prototype/` is the original JS prototype (`index.html` generator half + `microtubules.js`): the
**reference implementation** the core is ported from and tested against (`tests/parity`,
`spec/golden`), so its generator changes only deliberately (see [spec/PORT.md](../spec/PORT.md)).
Algorithm notes: [spec/ALGORITHM.md](../spec/ALGORITHM.md).

`prototype/scope/` is the JS imaging reference (the C++ imaging path mirrored; `tests/parity/scope_parity.mjs`).
`lab/` is the iteration page for prototype changes (A/B against main, imaging preview, metrics; `node
web/lab/serve.mjs`): try ideas there in JS and port to C++ only at merge, see [lab/README.md](lab/README.md).

Tools (`cd tools && npm install` once, or a global Playwright):
- `node tools/bench_pan.mjs [page]` -- pan benchmark (300 frames, headless Chromium, software GL).
- `node tools/check_cellfield_microtubules.mjs` -- the prototype's microtubule regression check.
