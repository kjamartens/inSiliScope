# Origami reference model

The model behind the Nanorulers specimen, as it was explored before the port (2026-10-12):

- `origami_bend.py`: maps points of a straight DNA-origami shape onto a thermally bent one (a 2D, surface-equilibrated
  worm-like chain: per segment `dtheta = N(0, 1) sqrt(ds / Lp)`, exact circular arcs, the "aligned" frame, a random
  roll about the rod's axis) and places the binding sites (rings of strands around the rod).
- `test_origami_bend.py`: its checks against theory (end-angle RMS `sqrt(L/Lp)`, tangent correlation `exp(-s/2Lp)`,
  group distances vs the 2D WLC) and a demo figure. Run from any directory (writes `origami_bend_demo.png` there):
  `python tools/origami_reference/test_origami_bend.py` (~7 s).
- `crosscheck.py`: feeds `origami_bend.py` the bending normals of the JS/C++ model and returns its sites;
  `node tools/origami_reference/freeze_crosscheck.mjs` freezes them for three molecules into
  `tests/parity/origami_bend_ref.inc`, and `world_checks.origami` checks the C++ places every site at the same point
  (to 1e-9 nm; ~1e-14 measured). Re-freeze after a deliberate change of the rod model.

The simulation's model is `web/prototype/scope/origami.js` (JS reference) and `core/src/origami.*` (C++): the same
geometry, with the sequential numpy stream replaced by address-based pcg4d draws (pure functions of seed, tile and
molecule). Its defaults differ from the scripts' (`Lp` 1500 nm instead of 273.1 nm; see `data/origami/designs.json`
and the docs' Samples / Nanorulers page).
