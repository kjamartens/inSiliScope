# Imaging reference (JS)

The JS twin of the C++ imaging path: a cell field's dyes, their blinks, the PSF, the frame render, the
camera, WideField and BrightField. Together with the geometry prototype (`../index.html`, `../microtubules.js`) it is
the **reference implementation** you iterate on in the lab (`web/lab/`); the C++ is ported from it when a
change merges to main. ES modules, no build, Node or browser.

| JS | mirrors (C++) | what |
|---|---|---|
| `rng.js` | `core/src/rng.h`, `Simulation/SMLMCounterRng.h`, `std::mt19937_64` + `SMLMNoise.cpp` `GaussianRng` | address hashes, camera-noise draws, per-pixel map stream |
| `dyes.js` | `core/src/dyes.cpp`, `BuildMtFrames` (`core/src/microtubules.cpp`) | lattice sites, labels (density, fluorescent fraction, mode dSTORM / PALM / DNA-PAINT / WideField, orientation), blink schedules and continuous windows, persistent (DNA-PAINT) sites |
| `world.js` | `core/src/world.cpp` | fixed 8x8-chunk packing blocks, cell assets, 1 um dye blocks, `STRUCTURES` and one label each, `eventsInWindow`, `continuousInWindow`, `density3d` by structure, `opticalVolume` (same answers, same event order) |
| `spectra.js` | `Simulation/Spectra.*` (issue 16 port) | spectra on 300-900 nm, ideal filters, cross section, laser photon flux, detected fraction, effective wavelength |
| `dye_library.js` (+ generated `dye_library_data.js`) | `Simulation/DyeLibrary.*`, `LightPath.*` (issue 16 port) | dyes from `data/dyes`, slots and overrides, the light path, per-state photophysics, the world label's kinetics, the free-imager background |
| `fluorescence.js` | `ScopeMovie.cpp` FluorescenceMovie (issue 16 port) | every label in its mode: blinks per (structure, state) PSF group, continuous populations mean-field or per dye (running image), imager background |
| `psf.js` | `Simulation/ZernikePsf.cpp`, `SMLMZernike.cpp`, `FftRadix2.h`, `PsfGeneratorBridge.cpp` (kernel cache, block sums, splat, Fft placement) | Gibson-Lanni + Zernike + double-helix PSF, sub-pixel placement |
| `render.js` | `SMLMSimulation.cpp` `RenderPhotonImage`/`BucketEventsByFrame`, `SMLMNoise.cpp` | frame photons, noise maps, `ApplyNoiseChain` (sCMOS, EMCCD) |
| `widefield.js` | `WidefieldRender.cpp` (CPU path) | the mean-field image of a structure's dyes: dye planes on world tiles, PSF-plane deposition, FFT convolution, sub-cell shift |
| `brightfield.js` | `BrightfieldRender.cpp`, `ScopeMovie.cpp` `ScopeBrightfieldSpec`/`RenderBrightfieldMovie` | refractive-index slices from the optical volume, margin taper, Abbe sources, thin / multislice propagation, detection pupil, defocus, binning |
| `scope_movie.js` | `ScopeMovie.cpp` + `CellFieldSource.cpp` | the option table (= `insiliscope_cli` options), setup, `renderScopeMovie` |

Same operations in the same order as the C++ (doubles; `Math.fround` where the C++ stores floats).
`node tests/parity/scope_parity.mjs` compares it with the committed C++ (`web/insiliscope_module.js`, no build):
events equal, SR movies 100% identical ADU, WideField >= 99.9% (the C++ convolves in float32), optical volume
bit-identical, BrightField >= 99.5% identical ADU with the intensity within 1e-4 (complex float32 FFTs; the
differences are single-electron Poisson flips). Not mirrored (speed only): caches, threads, the GPU paths, WideField
focus bands and spectra caches, BrightField's cache-blocked band-pruned FFTs.

```js
import { loadPrototype } from '../../../tests/parity/load_prototype.mjs';
import { renderScopeMovie } from './scope_movie.js';
const P = loadPrototype(indexHtmlText, microtubulesJsText);
renderScopeMovie(P, 'world-seed=1249 x=68 y=7 size=64 frames=20', (f, adu, photons) => { ... });
```

Changing the model: edit here, look at it in the lab (A/B against main's C++, which then differs on purpose),
note it in `PORT_PENDING.md`; the port brings the named C++ file in line until `scope_parity.mjs` passes again.
Not here: the RichardsWolf/GibsonLanni PSFs (JVM in the adapter), drift, the non-CellField patterns, live
mode's bleach field.
