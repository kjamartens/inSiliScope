# Validation

What is checked, against what, and where.

| Claim | Check | Where |
|---|---|---|
| RNG and geometry equal the JS prototype bit for bit | golden vectors frozen from the prototype (Node 24); native and WASM must match | ctest `golden_vectors`, `tests/parity/run.mjs`, CI `golden-fresh` |
| Math functions equal V8's | 40k sampled arguments vs `Math.*` | `tests/parity/run.mjs` |
| Determinism under any query history, tiling, threads | `world_checks` (query order, cache size, 8 threads = 1 thread) | ctest `world_checks` |
| Dye statistics | lattice angles and stagger, ring at 12.5 nm, tip at 24.5 nm, linker range, exponential ON/OFF means, geometric blink count, log-normal brightness | `world_checks` `KineticsStats` |
| Persistent sites | constant rate over 0-100 s and 10000 s, ON mean, window slicing | `world_checks` |
| Emitter density semantics | rate of blinks switching ON, live and precomputed | ctest `emitter_density` |
| Splat and FFT placement | against verbatim copies of the previous implementation; parallel = serial | ctest `sr_render` |
| Widefield engine | job format through a CPU reference host; GPU vs CPU (<= 3e-4 rms from fp16 spectra) | ctest `widefield`, `tests/web/wf_gpu_check.mjs`, `wf_gpu_d3d11` |
| Brightfield engine | weak phase grating (thin and 8 slices) vs first-order theory (~1e-5), empty field = 1, energy, serial = parallel; core optical volume: tiling, threads, nucleus volume = ellipsoids | ctest `brightfield`, `world_checks`, `tests/web/viewer_bf_movie.mjs` |
| C++ chirp-Z PSF (`ZernikePsf.cpp`) vs webSMLM's JS / the Java class | 0 / ~1e-12 relative L2 | ctest `zernike_psf`, `tools/psf_parity_check/` |
| Chirp-Z PSF vs webSMLM's own chirp-Z | n <= 6 Zernikes, double-helix mask, depth shift: 0.0000% relative L2 | `tools/psf_parity_check/` |
| Chirp-Z vs the old direct quadrature | 0.22-0.29% relative L2 | history in `docs/dev/` |
| PSF and noise models vs SMLM Challenge methodology | research comparison; the Challenge ground truth is a measured PSF table, so it gives no Zernike targets | `docs/dev/vectorial-psf-step4-smlm-challenge-comparison.md` |
| Adapter output unchanged by refactors | pixel hashes of fixed configurations | `tools/adapter_pixel_hash.py` |

## Known deviations

- The cytoplasm mesh uses `std::pow`, which matches JS only on the libm Node was built with (differences below 1e-6 um elsewhere).
- Brightfield: only cytoplasm, nucleus and microtubules make contrast (no nucleoli, vesicles, lipid droplets...: not
  simulated yet, kept out on purpose); scalar forward multislice, monochromatic, flat Koehler field; CellField only.
- Widefield: the Gaussian PSF (`psf-model=0`, not the default) ignores defocus; CellField only.
- Drift: the z drift of widefield and brightfield frames is interpolated between foci 10 nm apart; the background map
  does not move with the sample.
- Not yet verified by eye in every Micro-Manager configuration (headless pymmcore tests cover every property and the render path).
- Debug-only dye geometry in the JS prototype uses a sequential stream; the C++ hashing is normative and only the *statistics* match.
