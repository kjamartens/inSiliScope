# Notes for webSMLM: camera noise models in demoCam_SMLM_MM

**Audience**: a future Claude session working in
`C:\Users\koen-\Documents\GitHub\webSMLM` (single-file client-side app,
`webSMLM.html`). Written from *this* repo (`demoCam_SMLM_MM`, a C++
Micro-Manager device adapter) after extending its own camera noise model,
as a handoff — not a plan to execute here. Companion to
[`zernike-and-astigmatism-notes.md`](zernike-and-astigmatism-notes.md) in
this same folder (that one covers the PSF/aberration side; this one covers
the noise side). Read it, then re-scope against whatever webSMLM actually
needs; don't treat it as a locked spec.

**tl;dr**: both simulators started from the same noise chain shape
(Poisson shot noise → Gaussian read noise → gain → static per-pixel
offset), and until recently they were nearly identical. demoCam just added
three things on top: quantum efficiency, dark current, and optional
per-pixel sCMOS-style gain/read-noise maps — all small, purely-arithmetic
additions with no new architecture (no JVM/JNI involvement, unlike the
PSF-side work in the companion doc). All three are directly portable to
webSMLM's `generateSynthetic()` as straightforward formula changes. One
finding from demoCam's own research phase is *not* portable as a number:
demoCam looked into whether to add EMCCD EM-gain excess noise (a
Gamma-distributed multiplication stage) and found the physics doesn't
apply to sCMOS at all — skip it unless webSMLM ever wants to simulate an
EMCCD specifically.

## 1. What demoCam_SMLM_MM's noise model looks like now

Full details: [`../vectorial-psf-plan.md`](../vectorial-psf-plan.md)'s
"Noise model follow-ups" section (under Step 4) for the sourcing/reasoning,
and this repo's `CLAUDE.md` ("Noise model properties" section) for the
property list. Implementation:
[`../../DeviceAdapter/SMLMDemoCam/Simulation/SMLMNoise.h`](../../DeviceAdapter/SMLMDemoCam/Simulation/SMLMNoise.h)/`.cpp`.

Chain, in order, per pixel:

1. **Background** is added to the clean photon-count image before any of
   this (uniform scalar across the frame, same as webSMLM — see section 2).
2. **Quantum efficiency** (`QuantumEfficiency` property, 0–1): incident
   photons → mean *detected photoelectrons*, `meanElectrons = photons * QE`.
   New; previously QE was implicitly 1 (folded into whatever
   `PhotonsPerSecond` was set to).
3. **Dark current** (`DarkCurrentElectronsPerSec` property, converted to a
   per-frame electron count via the current exposure time): added directly
   to `meanElectrons` — deliberately *after* QE, not scaled by it, since
   dark current originates in the sensor itself, not in incident light.
   New; previously indistinguishable from background (same flat-scalar
   effect on the final image), even though the two have different physical
   origins (dark current scales with exposure/temperature, not
   illumination).
4. **Poisson shot noise** on `meanElectrons`, **plus additive Gaussian read
   noise** (in electrons) — one function call,
   `CombinedShotAndReadNoise(rng, meanElectrons, effectiveReadNoise)`,
   `PoissonRng`/Box-Muller under the hood, no `std::poisson_distribution`/
   `std::normal_distribution` object construction per pixel (that measurably
   mattered at hundreds of millions of calls for a full precomputed stack —
   worth knowing if webSMLM ever needs to simulate a very large stack and
   its own Poisson/Gaussian draws become a bottleneck).
5. **Division by gain** (photons/ADU, really e⁻/ADU once QE is in the
   picture — same "conversion gain" concept camera datasheets use).
6. **Static per-pixel additive offset** (`PixelOffsetMap`, mean + Gaussian
   spread, generated once per RandomSeed/frame-size and reused every frame
   — not resampled per frame, matching real fixed-pattern offset/DSNU).
7. Clamp to `[0, 65535]` (16-bit).

Steps 4 and 5's *effective* read noise and gain are no longer necessarily
one scalar for the whole sensor: two new optional structs,
`PixelReadNoiseMap`/`PixelGainMap`, hold a per-pixel value
`nominal*(1 + stdFraction*Gaussian())` — same "generate once, reuse every
frame" shape as `PixelOffsetMap` — modeling sCMOS-style pixel-to-pixel
variation (every sCMOS pixel has its own on-chip amplifier and ADC, so real
sCMOS sensors have real pixel-to-pixel gain/read-noise spread, unlike an
EMCCD's single shared electron-multiplying register). `stdFraction = 0`
(the `PixelGainStdPct`/`PixelReadNoiseStdPct` properties, both in percent)
reproduces the old scalar-everywhere behavior exactly, and `ApplyNoiseChain`
falls back to the scalar value whenever a map's size doesn't match the
current frame — same fallback pattern `PixelOffsetMap` already used.

**Reference defaults** (all four new/changed noise properties): Photometrics
Kinetix22 sCMOS, Sensitivity (CMS) mode datasheet — QE 0.85 (at 660 nm,
read off the published QE curve, not a table value), dark current 1.03
e⁻/pixel/s, conversion gain 0.25 e⁻/count, read noise 1.2 e⁻ rms. The two
per-pixel spread percentages (5% gain, 20% read noise) are *not*
datasheet-sourced — Photometrics doesn't publish actual per-pixel variance
— and are flagged as estimates in the plan doc. See the "Reference
defaults" table in `../vectorial-psf-plan.md` if webSMLM wants the same
numbers for a "realistic sCMOS preset."

**What demoCam looked at but did *not* add**: an EMCCD EM-gain
excess-noise (Gamma-distributed multiplication) stage — found during Step
4's SMLM Challenge research (the ground-truth simulator in Sage et al. 2019
uses exactly this term for its EMCCD data:
`Gamma(detected_electrons, EM_gain)` between shot noise and read noise,
adding roughly √2 extra effective variance at high gain versus a plain
deterministic multiply/divide). **This is EMCCD-specific physics — an
electron avalanche-multiplication register — with no sCMOS equivalent at
all**, so it's irrelevant unless webSMLM ever wants to simulate an EMCCD
specifically rather than a generic/sCMOS-like sensor. If that ever comes
up: don't reuse demoCam's *scalar* gain division as the model, add a real
Gamma-distributed stage (`Poisson → Gamma(shape=electrons, scale≈EM_gain)
→ Gaussian(read_noise)`), same location the Poisson/read-noise combination
currently happens.

**What demoCam still doesn't have either** (documented in the plan doc as
deprioritized, not because they're unimportant — just not requested as
follow-up work yet): pixel response non-uniformity/PRNU as a *separate*
concept from the sCMOS per-pixel gain map above (arguably the same
per-pixel gain map already covers what PRNU would add — worth deciding
explicitly rather than assuming, if this becomes relevant to either
project), flat background with no vignetting/illumination falloff, and
saturation/full-well modeled only as a 16-bit ADU clamp rather than analog
full-well capacity with blooming, with bit depth hardcoded rather than a
configurable property.

## 2. What webSMLM currently does (read from `webSMLM.html` directly, not assumed)

`generateSynthetic()`, `webSMLM.html` around line 3458-3533:

- **Background** (`bg`, `PARAMS.simbg`): flat scalar, `img.fill(bg)`,
  same as demoCam — Poisson noise applies to background exactly like
  signal.
- **Noise chain** (line ~3524-3527), one line per pixel:
  ```js
  const adu = offsetMap[i] + (poisson(img[i]) + readNoiseE*gauss()) / simGain;
  img[i] = adu < 0 ? 0 : adu;
  ```
  i.e. `Poisson(photons) → + Gaussian(0, readNoiseE) → / simGain →
  + offsetMap[i] → clamp ≥ 0` (no upper clamp visible in this function —
  worth checking downstream, e.g. `makeStack`/TIFF export, if that matters
  for a port). Structurally identical to demoCam's *old* pre-this-work
  chain (steps 4/6/7 in section 1 above, minus 2/3/5's QE, dark current,
  and per-pixel maps).
- **Properties** (`PARAMS.simulation_*`, line ~2705-2722, UI at
  ~2005-2008): `simulation_gain` (photons/ADU, default **0.34** — notably
  already in the same ballpark as demoCam's new Kinetix-derived 0.25
  default, for what it's worth as a rough cross-check), `simulation_offset`
  (ADU, default 100 — same as demoCam's default), `simulation_offset_std`
  (ADU, per-pixel, default 3), `simulation_readnoise` (e⁻, default 2.7 —
  somewhat higher than demoCam's new Kinetix-derived 1.2 e⁻ default, but
  both are user-adjustable, not hard limits).
- **No quantum efficiency, no dark current, no per-pixel sCMOS gain/read-noise
  maps.** Same three gaps demoCam had before this round of work — webSMLM's
  `simulation_readnoise`/`simulation_gain` are exactly the "implicitly
  QE=1, dark current folded into background" scalars demoCam's old code
  was too (see section 1's "New; previously..." notes).
- **The docstring at line ~2020-2027 already explains the noise chain's
  ordering rationale** (read-noise-before-gain, offset-std/read-noise
  combining in quadrature for the Gain/offset-estimation self-consistency
  test) in essentially the same terms this doc uses for demoCam's chain —
  the two projects converged on the same physical model independently,
  which is a reasonable sign the ordering itself doesn't need
  reconsidering, just extending.

## 3. Recommended scope if porting this to webSMLM

All three additions are pure arithmetic — no architecture, no new
dependencies, and (unlike the PSF/Zernike work in the companion doc)
nothing JVM/native-specific to *not* port. Suggested order, cheapest/
highest-value first:

1. **Quantum efficiency**: one new `PARAMS` entry (`simulation_qe`,
   0-1, e.g. default ~0.85 to match a modern sCMOS), applied as
   `meanElectrons = img[i] * simQE` right before the existing
   `poisson(...)` call at line ~3525. Two-line change.
2. **Dark current**: one new `PARAMS` entry (`simulation_darkcurrent`,
   e⁻/pixel/s), converted to a per-frame count via whatever exposure-time
   equivalent webSMLM's simulator uses (check how `n`/frame duration is
   tracked — demoCam's `frameDurationSec` doesn't have an obvious webSMLM
   analogue from this read; may need to derive one, or just treat the
   property as "e⁻/pixel/frame" directly if webSMLM's simulator has no
   explicit per-frame duration concept — check before assuming). Added to
   `meanElectrons` after QE, same reasoning as demoCam's step 3.
3. **Per-pixel sCMOS gain/read-noise maps**: two new `Float32Array(w*h)`
   maps built the same way `offsetMap` already is (line ~3507-3508 is the
   template — `nominal*(1 + stdFraction*gauss())` per entry, generated once
   before the per-frame loop), two new `PARAMS` entries for the stdFraction
   (as a percent, matching demoCam's `PixelGainStdPct`/
   `PixelReadNoiseStdPct` naming/convention if consistency across the two
   projects is valued), then swap the scalar `simGain`/`readNoiseE` at line
   ~3525 for the per-pixel map lookups. `stdFraction = 0` (or simply
   omitting the feature) reproduces today's exact behavior.

None of these need webSMLM's `poisson()`/`gauss()` helpers to change
signature — same per-call pattern as today, just called with a
QE/dark-current-adjusted mean and (optionally) a per-pixel rather than
scalar read-noise argument.

**Skip**: EM-gain excess noise (Gamma stage) — see section 1's "did *not*
add" note — unless webSMLM specifically wants an EMCCD simulation mode,
which nothing in webSMLM's current UI/properties suggests it's aiming for
(no gain-register/EM-gain-style property exists today, just the generic
`simulation_gain`).

## 4. Reference map

**In `demoCam_SMLM_MM`** (this repo):

- `docs/vectorial-psf-plan.md` — "Noise model follow-ups" section (under
  Step 4) for the full sourcing/reasoning and the Kinetix22 reference-value
  table.
- `docs/vectorial-psf-step4-smlm-challenge-comparison.md` — the Sage et al.
  2019 SMLM Challenge research that surfaced the EM-gain excess-noise
  finding (section 3 of that doc).
- `CLAUDE.md` — "Noise model properties" section: quick summary + property
  names/defaults.
- `DeviceAdapter/SMLMDemoCam/Simulation/SMLMNoise.h` — the noise-chain API
  and its ordering doc comment (source of truth for the exact per-pixel
  formula and stage order).
- `DeviceAdapter/SMLMDemoCam/Simulation/SMLMNoise.cpp` — the implementation,
  including `PixelGainMap`/`PixelReadNoiseMap::Generate()`.

**In `webSMLM`**:

- `webSMLM.html` line ~3458 (`generateSynthetic`) — the simulator to
  extend.
- `webSMLM.html` line ~3505-3527 — the current offset-map construction and
  per-pixel noise chain, to be extended per section 3 above.
- `webSMLM.html` line ~2705-2722 (`PARAMS.simulation_*`) — existing noise
  property definitions to follow the naming/style convention of when adding
  `simulation_qe`/`simulation_darkcurrent`/new per-pixel-map stdFraction
  properties.
- `webSMLM.html` line ~2005-2008, ~2020-2027 — the Simulation panel UI and
  its explanatory docstring, to extend alongside any new properties.

**External**: [Photometrics Kinetix22 datasheet](https://sg-science.jp/product/pdf/Photometrics/Kinetix22-Datasheet_2024May9.pdf)
— source for demoCam's new default values, if webSMLM wants the same
reference numbers.
