# Cell-field viewer

Pan/zoom/tilt viewer for the infinite cell field. Open `index.html` directly in a browser -- no build
step, no server.

Today this is still the **JS prototype**: `index.html` (generator above the `// ---- viewer ---`
marker, viewer below it) and `microtubules.js` generate the field themselves, in Web Workers. It is
the **reference implementation** the C++ core is ported from and tested against (`tests/parity`,
`spec/golden`), so generator changes here must be deliberate (see [spec/PORT.md](../spec/PORT.md)).

In M3 the generator half is replaced by the WASM build of `core/` (workers instantiate the module;
the viewer keeps only UI/WebGL). Algorithm notes: [spec/ALGORITHM.md](../spec/ALGORITHM.md).

`tools/check_cellfield_microtubules.mjs` is the prototype's Playwright regression check for the
microtubule generator (`cd tools && npm install` once, then `npm run check:mt`).
