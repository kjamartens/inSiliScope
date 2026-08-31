# Step 4: Comparison against the SMLM Challenge (Sage et al. 2019) simulator

Research-only deliverable for Step 4 of `docs/vectorial-psf-plan.md`. Compares
this plugin's current PSF and noise models against the ground-truth simulator
used in D. Sage et al., "Super-Resolution Fight Club: Assessment of 2D and 3D
Single-Molecule Localization Microscopy Software," *Nature Methods* 16(5),
387-395, 2019.

## 1. Sources used

- **Primary source (full text, open access):** PMC copy of the paper —
  https://pmc.ncbi.nlm.nih.gov/articles/PMC6684258/ (PMC6684258). This is the
  NIH-hosted, publicly accessible full text of the Nature Methods paper
  (including its Online Methods), obtained via `WebFetch`. Quotes and numbers
  below are drawn from this copy unless noted otherwise.
- **Publisher page (paywalled, not accessed for full text):**
  https://www.nature.com/articles/s41592-019-0364-4
- **Preprint (bioRxiv, same authors/study, earlier version):**
  https://www.biorxiv.org/content/10.1101/362517v4.full — found via search but
  the fetch returned HTTP 429 (rate-limited); **not read**. Left here for a
  future retry if more detail is needed (e.g. Supplementary Note text not
  captured by the PMC excerpt tool).
- **Challenge website / data archive:** `http://bigwww.epfl.ch/smlm/challenge2016/`
  and `https://bigwww.epfl.ch/sage/` — both attempted via `WebFetch` and both
  failed with a TLS certificate error (`unable to verify the first
  certificate`) from this environment; **not read**. The paper text states the
  simulated datasets and generation parameters are hosted at
  `http://bigwww.epfl.ch/smlm/challenge2016/`, so that page is very likely the
  authoritative source for exact per-modality parameter files, but it could
  not be retrieved here.
- **Related preprint (2015 predecessor paper, same simulator lineage), not
  accessed:** `https://bigwww.epfl.ch/preprints/sage1501p.pdf` — same TLS
  error, not read.
- I did **not** access the paper's formal "Supplementary Information" PDF
  directly (Nature Methods hosts SI as a separate PDF from the main article,
  and PMC's HTML mirror may or may not inline all of it). The Online
  Methods content quoted below came through the PMC full-text fetch, which
  appears to include Methods but I cannot confirm it included every
  Supplementary Note in full (e.g. no explicit astigmatism cylindrical-lens
  specification or NA/immersion value turned up, which a supplementary
  methods PDF might contain).

**Honesty check on gaps:** I was not able to confirm (from any source reached
here) the objective NA/immersion medium used for the ground-truth PSF
measurement, the specific optical means used to generate astigmatism (e.g.
cylindrical lens focal length), or the double-helix phase mask design
specifics beyond "designs supplied by R. Piestun / Double Helix LLC." Where a
number below is not a direct quote, it is flagged as such.

## 2. PSF comparison

### 2.1 What the SMLM Challenge simulator actually did

This is the single most important finding of this step, and it changes the
framing of Step 5: **the SMLM Challenge ground truth did not use a vectorial
optical model (PSFGenerator or otherwise) at all.** Per the paper (PMC full
text):

> "Model PSFs, stored as high resolution look up tables, were derived from
> experimentally measured PSFs."

The measurement/construction process, as described:

- PSFs were acquired by imaging 100 nm Tetraspeck beads (Invitrogen) on
  #1.5 coverglass, in water.
- Bead images were interpolated to a 10 nm lateral pixel size, co-aligned by
  cross-correlation, and averaged across 3-6 beads.
- "A central Z-range of 1.5 μm was selected that represents 151 optical
  planes with a Z-step of 10 nm" (i.e. a measured 3D PSF volume at 10x10x10 nm
  voxels, spanning -750 nm to +750 nm) — this becomes the lookup table
  emitters are resampled from at render time.
- Excitation/emission: 640-647 nm excitation with a Cy5 emission filter.
- Field of view for the simulated data: 6.4 x 6.4 μm².

Per modality:

- **2D and astigmatic 3D:** used directly from the experimentally measured
  PSF stack (astigmatism was a property of the real optical path used to
  *acquire* the calibration beads — e.g. a cylindrical lens in the real
  microscope — not something added synthetically after the fact). I could
  not confirm the specific optical element/strength used to introduce the
  astigmatism from the sources reached in this step.
- **Biplane 3D:** explicitly **semi-synthetic**, built from the *same*
  measured 2D PSF: "We semi-synthetically constructed a realistic biplane PSF
  from the experimental 2D PSF ... by duplicating the 2D PSF and offsetting
  it by -250 nm and +250 nm for each Z-plane." I.e. two shifted copies of one
  in-focus measured PSF stack, not two independently measured/simulated focal
  planes.
- **Double-helix 3D:** experimentally measured PSF used directly, with the
  physical phase mask supplied by the DH-PSF inventors (R. Piestun's group /
  Double Helix Optics LLC); reported transmission efficiency ~96%.

So there is **no Zernike-coefficient parameterization anywhere in the ground
truth** — the "aberration" for astigmatic/biplane/DH PSFs is baked into a
measured 3D intensity lookup table, not expressed as pupil-phase coefficients
this plugin's future Zernike step could directly mimic by number.

### 2.2 Versus this plugin's current approach

This plugin (`Simulation/PsfGeneratorBridge.h/.cpp`) does the opposite:
computes a PSF *ab initio* from a vectorial optical model —
`RichardsWolfPSF`/`GibsonLanniPSF` from EPFL's own PSFGenerator, run in an
embedded JVM — parameterized by NA, wavelength, immersion/sample index, and
(for Gibson-Lanni) sample depth/working distance, at oversampling factor 12x
(default `PsfOversampling`), default NA 1.4 / wavelength 660 nm
(`PsfGeneratorBridge.h`), then "oversample once, downsample everywhere"
(`SplatPsfKernel`). It has no aberration model yet (Step 5's job), and no
biplane or double-helix rendering mode at all — both of those are 3D-encoding
*optical schemes* (a beam-splitter to two detector planes; a DH phase mask in
the Fourier plane) that are architecturally different from a single-path
vectorial PSF kernel, not just a Zernike coefficient choice.

**Implication for Step 5:** Since the Challenge paper doesn't supply Zernike
coefficients (it never expressed its aberrations that way), Step 5's
astigmatism magnitude cannot be *directly matched* to the paper's numbers —
there's nothing numeric to match. The paper is still useful context for
*why* astigmatic/DH/biplane PSFs matter (it's literally the community
benchmark these modalities are judged against) and for order-of-magnitude
plausibility, but the actual Zernike coefficient values for Step 5 will have
to come from a different source: e.g. typical published cylindrical-lens
astigmatism strengths in the 3D-STORM literature (Huang et al. 2008 used
roughly 40-45 diopter... — not verified in this step, would need separate
lookup), or simply chosen so the axial range over which the PSF
elongates/splits visually matches a plausible ~600 nm-1 μm axial encoding
range, consistent with this plugin's own `PsfZRangeUm` default of 2.0 μm.
**This is a gap to flag explicitly rather than paper over with an invented
number attributed to Sage et al.**

## 3. Noise comparison

### 3.1 What the SMLM Challenge simulator used

The paper describes an explicit EMCCD noise chain with three stochastic
terms — shot noise, EM-gain (multiplication) noise, and read noise — modeled
as (quoting the equations extracted from the PMC text):

> σS = shot noise (photon/photoelectron shot noise from signal + background)
> σR = read noise, "described by second moment of the Gaussian distribution"
> σEM = electron multiplication noise, "described by the second moment of
> the Gamma distribution"

and the per-pixel generative chain:

```
nie   = Poisson(QE * n_photons_in + c)
noe   = Gamma(nie, EMgain) + Gaussian(0, sigma_R)
ADUout = clip( floor((noe) / e_adu) + BL , 0, 65535 )
```

(reconstructed from the extracted equation text; treat the exact
floor/mod placement as approximate — the underlying point, an explicit
**Gamma-distributed EM-gain excess-noise stage between Poisson shot noise and
Gaussian read noise**, is the important, directly-quoted structural fact.)

Reported camera parameters (Photometrics Evolve Delta 512 EMCCD):

| Parameter | Value |
|---|---|
| Quantum efficiency (QE) | 0.9 |
| Read noise σR | 74.4 e⁻ |
| Spurious charge c | 0.002 e⁻ |
| EM gain | 300 |
| e⁻/ADU (`eadu`) | 45 |
| System gain G | 6 (one fetch reported "0.9" for this field — likely a
extraction/OCR inconsistency between two passes; **not fully resolved**, flagged as uncertain) |
| Baseline offset BL | 100 ADU |

A second `WebSearch` pass (not the primary paper fetch, so treated as
secondary/lower-confidence) corroborates the general EMCCD noise structure
independently: "EM-CCD cameras introduce an additional amplification stage,
modeled as a Gamma distribution... read noise (both for sCMOS and EMCCD
cameras) is approximated by a zero mean additive Gaussian distribution," and
for sCMOS: "the read noise and gain factor varies significantly across the
pixel array in sCMOS cameras, and may not be considered a constant value for
a given pixel" — i.e. per-pixel sCMOS gain/read-noise maps are the norm in
the broader literature this paper sits in, even though the Challenge's own
ground-truth datasets (per the directly-fetched text) used only the single
EMCCD camera model above, with **no sCMOS ground-truth parameters found in
the accessible text** — the paper does mention sCMOS only in passing
("... such as scientific CMOS cameras") without giving sCMOS simulation
parameters.

### 3.2 Versus this plugin's current chain

`Simulation/SMLMNoise.cpp`'s `ApplyNoiseChain` does:

1. Poisson shot noise on the photon image (`PoissonRng`, exact for mean < 10,
   Gaussian-approximated above that).
2. A single **scalar, additive Gaussian read-noise** term folded into the
   same draw (`CombinedShotAndReadNoise`) — no per-pixel variation.
3. Division by a single **scalar gain** (`gainPhotonsPerAdu`) — i.e. a
   fixed photons-per-ADU conversion, not a Gamma-distributed EM multiplication
   stage.
4. Addition of a **static per-pixel offset map** (`PixelOffsetMap`, mean +
   Gaussian-distributed per-pixel spread, generated once) — this is the one
   piece of per-pixel spatial structure already present, roughly analogous to
   a fixed-pattern baseline/offset non-uniformity, but with no accompanying
   per-pixel *gain* or *read-noise* non-uniformity.

Concrete gaps versus the paper's own EMCCD model:

- **No EM-gain excess-noise (Gamma) stage.** This plugin's scalar gain step
  is a plain division, i.e. equivalent to modeling an EMCCD with EM gain but
  *without* its characteristic excess noise factor (~√2 extra effective
  read-noise-equivalent variance at high gain) — a real, well-known
  difference from actual EMCCD statistics that the Challenge simulator
  explicitly includes via the Gamma term.
- **No per-pixel sCMOS gain/read-noise maps.** Not present in either this
  plugin or (per what could be confirmed) the Challenge's own ground truth —
  so this is not a discrepancy versus the paper specifically, but is a
  known gap versus sCMOS reality broadly (and the paper's own text
  acknowledges sCMOS has this property, even though its own datasets didn't
  use it).
- **Different absolute noise scale.** This plugin's read-noise/gain
  properties are user-configurable with no camera-specific fixed default
  called out in the files reviewed here; the paper's fixed Evolve Delta 512
  numbers (σR = 74.4 e⁻, EM gain 300, 45 e⁻/ADU, 100 ADU baseline) are a
  concrete, citable target if a "realistic EMCCD preset" property/default is
  ever wanted for closer apples-to-apples comparison against Challenge-style
  data.

## 4. Recommendations

Ranked by how directly they're supported by what was actually found (not
invented) in this step:

1. **Do not attribute specific Zernike coefficient values to Sage et al. —
   there aren't any.** The ground truth is a measured PSF lookup table, not a
   parametric aberration model. Step 5's Zernike defaults will need a
   different citation. Suggested path: pick standard OSA/ANSI single-index
   mode **5** (vertical astigmatism, `Z_2^2`) as the primary Step 5 test
   target (matches typical 3D-STORM/astigmatism-based SMLM), with coma
   (7/8) as a secondary sanity-check mode for asymmetric aberration — but
   treat any specific RMS wavefront magnitude (e.g. "0.1 λ RMS
   astigmatism") as an *estimate to be tuned visually*, not a value sourced
   from this paper. **Correction (post-implementation):** this note
   originally said "index 6" — working the actual OSA/ANSI formula
   (`j = n(n+1)/2 + l`) during Step 5's implementation showed vertical
   astigmatism is index 5, not 6 (index 6 is oblique trefoil); see
   `Simulation/SMLMZernike.h`'s doc comment for the verified mapping and
   `docs/vectorial-psf-plan.md`'s Step 5 section for the full
   implementation writeup. This should be stated explicitly in Step 5's own
   documentation/commit message so a future reader doesn't mistake either
   number for a paper-derived one.
2. **Biplane and double-helix are architecturally out of scope for the
   current Zernike-only Step 5 plan**, and this is now confirmed rather than
   assumed: the paper's own biplane PSF is *two shifted copies of one PSF*,
   not one aberrated pupil function — i.e. modeling it faithfully would need
   a "second detection path with axial offset" feature (a rendering-pipeline
   change, splatting each emitter twice at ± some z-offset with a shared
   photon budget) rather than a Zernike coefficient. DH-PSF likewise needs an
   actual Fourier-plane phase-mask propagation model (a double-helix rotating
   double-lobe pattern is not reachable via low-order Zernike terms at
   reasonable magnitude). Both are worth flagging as *possible future steps
   beyond Step 5*, not something the planned Zernike-only patch will produce.
3. **If exact apples-to-apples EMCCD noise realism vs. Challenge-style data
   is ever wanted**, add an explicit EM-gain Gamma-distribution excess-noise
   stage to `SMLMNoise.cpp` (between shot noise and the current scalar
   gain/read-noise draw), and consider an optional "Evolve Delta 512" noise
   preset using the paper's own numbers (QE 0.9, read noise 74.4 e⁻, EM gain
   300, 45 e⁻/ADU, baseline 100 ADU) as fixed property defaults for a
   `NoisePreset=EMCCD_ChallengeStyle` style property. This is a bigger lift
   than Step 5 and should be its own follow-up, not folded into the Zernike
   work.
4. **Per-pixel sCMOS gain/read-noise maps** remain a real gap versus
   physical sCMOS sensors generally (confirmed by secondary source, not
   contradicted by the Challenge paper, which itself didn't simulate sCMOS)
   — worth a future property (`OnPixelGainMap`-style, analogous to the
   existing `PixelOffsetMap`) if sCMOS realism becomes a priority, but this
   is independent of both Step 4 and Step 5 and not urgent for the
   Zernike-PSF work specifically.

**Overall for Step 5 planning:** the paper does not hand Step 5 numeric
Zernike targets — its most useful contribution to Step 5 is *scope
clarification*: astigmatism is the only one of the three "3D modalities" that
a single-pupil Zernike patch to `RichardsWolfPSF` can plausibly reproduce;
biplane and double-helix are structurally different techniques that Step 5
as currently scoped will not address, and that should be documented as an
explicit known limitation rather than silently left implicit.
