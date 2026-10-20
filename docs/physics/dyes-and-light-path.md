# Dyes and light path

Since issue 16 every structure carries a **label**: a dye of the library in one of four modes, imaged through a light
path of lasers, a dichroic, an emission filter and the camera's QE curve. What a dye emits, at which wavelength (and so
with which PSF), and how fast it switches all follow from the dye's data and the light path; nothing is a free
"photons per second" number any more. Only the microtubules carry a label today. The switching models themselves are on
[Photophysics](photophysics.md).

## The dye library

`data/dyes/library.json` holds the dyes and fluorescent proteins: AF647, AF532, Cy5, ATTO 655, ATTO 542, mEos3.2,
Dendra2, PAmCherry2, mEGFP, mScarlet, DAPI, Hoechst 33342 and a `Custom` dye with Gaussian spectra. Spectra, extinction
coefficients and quantum yields come from FPbase [[lambert2019](../references.md#lambert2019)] (CC BY-SA 4.0); per-mode
kinetics come from the literature named in each entry (dSTORM from [[dempsey2011](../references.md#dempsey2011)],
DNA-PAINT from [[jungmann2010](../references.md#jungmann2010), [schnitzbauer2017](../references.md#schnitzbauer2017)],
PALM photoactivation efficiencies from [[durisic2014](../references.md#durisic2014)], primed conversion from
[[dempsey2015](../references.md#dempsey2015), [turkowyd2017](../references.md#turkowyd2017)]). Values without a
measurement behind them are marked *estimate* in the data. [References](../references.md) lists every source.

A dye has a default mode and a block per mode it supports. The **fluorescent fraction** (e.g. the share of a PALM
protein that ever photoactivates) thins the labelled sites with its own address-based draw, nested in the labelled set.

Three **dye slots** (`dye1..3.source`, MM `Fluorophores.Dye{1,2,3}_Source`) hold edited copies of library dyes; a structure
can use a slot (`mt-dye=Dye1`). Any dye field can be overridden per structure (`mt-dye.on-sec=0.03`) or per slot
(`dye1.qy=0.5`). Micro-Manager shows the slots as `Fluorophores.Dye<N>_*` (Expert), loaded from the library when their
source changes and editable after; a structure there takes a slot (`CellField.Microtubules_Label = Dye1`) instead of
per-structure overrides.

## Label modes

| Mode | Emitters | Rendered as |
|---|---|---|
| dSTORM | an initial ON phase (all dyes on, Exp(`initial-on-sec`)), then blinks until bleached | blinks; the initial ON as a continuous population |
| PALM | optional pre-converted state (e.g. mEos3.2 green) until activation, then blinks | blinks; the pre state as a continuous population |
| DNA-PAINT | persistent binding sites: imager binds at \(k_{on} c\), never bleaches (\(k_{on}\) and the bound time are the target's: [photophysics](photophysics.md#dna-paint)) | blinks, plus the free imager's background |
| WideField | every dye emits from \(t=0\) and bleaches by its photon budget | a continuous population |

Changing the mode sets the labelled share of the binding sites to the mode's suggestion (DNA-PAINT 70 %, dSTORM 3 %,
PALM 25 %, WideField 70 %; *estimates* of typical densities), as the viewer does.

The four modes on one field:

<!-- fig:light-modes -->

## Excitation, emission and detection

For each dye state (main, or a PALM pre state) and the light path:

\[ k_{exc} = \sum_{\text{lasers}} \sigma(\lambda)\; \Phi(\lambda)\;\bigl(1 - T_{D}(\lambda)\bigr), \qquad
   \sigma(\lambda) = \frac{\ln 10\;10^{3}\,\varepsilon\,E(\lambda)}{N_A} \]

with \(\varepsilon\) the extinction coefficient, \(E\) the normalised excitation spectrum, \(\Phi\) the laser's photon
flux (from its intensity in kW/cm\(^2\)) and \(T_D\) the dichroic's transmission (a long-pass reflects the laser onto
the sample). \(\sigma\) is the Beer-Lambert cross-section: \(10^{-\varepsilon c l} = e^{-\sigma n l}\) with
\(n = 10^{-3} c N_A\) molecules per cm\(^3\) for \(c\) in mol/L. The emission rate is \(k_{em} = \mathrm{QY}\,k_{exc}\).
The detected fraction of the emission spectrum \(S\) is

\[ F = \frac{\sum S\,T_D\,T_F\,\mathrm{QE}}{\sum S}, \]

through the dichroic, the emission filter \(T_F\) and the camera's QE curve, and the detected photons per second while
emitting are \(k_{em}\,\eta\,F\) with the objective's collection efficiency
\(\eta = \tfrac12\bigl(1 - \sqrt{1 - (\mathrm{NA}/n)^2}\bigr)\), the share of isotropic emission inside the objective's
cone (\(\sin\theta = \mathrm{NA}/n\)). The PSF of each state is computed at the detected-spectrum-weighted wavelength,
rounded to 2 nm (one kernel per distinct wavelength). MM shows the microtubules' main state as read-only properties: `Fluorophores.Microtubules_DetectedPct` (\(100F\), *without* \(\eta\)),
`EffectiveEmissionNm` and `PhotonsPerSecOn` (\(k_{em}\eta F\)).

The four typical labels through their light presets, as a movie resolves them:

<!-- fig:light-spectra -->

Because the QE is in \(F\), the camera noise chain runs at QE 1 for fluorescence; the flat `background-per-sec` is
multiplied by the QE at the filter's centre. BrightField uses the curve's QE at its lamp wavelength.

**Activation and switching rates** follow the lasers too: dSTORM switches on at \(1/\tau_{off} + a_{405} I_{405}\), and
its ON, spontaneous-dark and initial-ON times scale as \(k_{exc,ref}/k_{exc}\), so the photons per blink and the duty
cycle stay as measured at Dempsey et al.'s intensities (488 nm 1.2, 561 nm 2.2, 647 nm 0.8 kW/cm\(^2\))
[[dempsey2011](../references.md#dempsey2011)] (\(a_{405}\) set from the 405 nm sensitivity grades of its Table 2). PALM
activates at the spontaneous rate plus \(a_{405} I_{405}\) plus, for primed conversion, a term in the product of the
470-510 nm and 690-780 nm intensities [[dempsey2015](../references.md#dempsey2015),
[turkowyd2017](../references.md#turkowyd2017)] (the PALM rate coefficients are *estimates*). DNA-PAINT blinks at
\(k_{on} c\) [[jungmann2010](../references.md#jungmann2010)] (\(k_{on}\) of the order of \(10^6\) /M/s, an *estimate*),
with \(c\) the imager concentration (`mt-imager-nm`, MM `CellField.Microtubules_ImagerNanoMol`).

## Light-path presets

`data/dyes/light_path.json` defines the lasers (405, 488, 561, 640, 730 nm and a custom line), dichroics, emission
filters and presets: `dSTORM-640/561/488` (Dempsey's conditions [[dempsey2011](../references.md#dempsey2011)]),
`PAINT-640/561/488`, `PALM-561`, `PALM-primed`, `WF-405/488/561/640`. Intensities without a source are *estimates*.
Every dye mode names its preset. `light-preset=auto` (the default, cli/viewer; MM `Lasers.Preset` `Auto`) combines
the presets of the labelled targets of the mounted sample: each laser at its largest power among them, the dichroic and
filter where they all agree, else a multi-band set (Chroma ZT405/488/561/640rpc dichroic and ZET405/488/561/640m
emission filter, FPbase curves [[lambert2019](../references.md#lambert2019)]), so one image shows every target with
its real cross-talk. A dye or mode change re-applies it (viewer, MM). With one labelled target that is its own preset,
e.g. `PAINT-640` (640 nm at 1 kW/cm\(^2\), an *estimate*; LP650, 676/37) for the microtubules' ATTO 655 DNA-PAINT
label alone. The default cell field labels two targets: the microtubules (ATTO 655, `PAINT-640`) and the nuclear
pores (Cy3B, `PAINT-561`), so 561 and 640 nm through the multi-band set. Camera presets
(`cameras.json`: Kinetix22, iXon Ultra 897, ...) set the noise values and the QE curve.

## Excitation (laser clean-up) filters

An excitation filter sits between the lasers and the dichroic (`ex-filter`, MM `ExcitationFilter` wheel or the
`FilterCube`). Each laser line's intensity is multiplied by the filter's transmission at that line, so a 640 nm
clean-up blocks the 561 nm line, as on a real scope. The library has `None` (the default, and every preset's),
ideal laser-line band passes of +/- 5 nm (`BP405-10`, `BP488-10`, `BP561-10`, `BP640-10`), an ideal quad (`Quad`),
measured curves from FPbase [[lambert2019](../references.md#lambert2019)] of Chroma ZET405/20x, ZET488/10x,
ZET561/10x, ZET642/20x and the quad ZET405/488/561/640xv2, and Semrock FF01-405/10, FF01-488/10, FF01-561/14 and
FF01-640/14, and a `Custom` band pass (`ex-lo-nm`, `ex-hi-nm`). Semrock's MaxDiode/MaxLine clean-ups (LD01-405/10,
LL02-561, ...) have no FPbase curve; the FF01 band passes stand in for them.

## Which light: the shutters

The lasers (epi) and the transmitted lamp each have a shutter: `light-epi` and `light-trans` (1 open, 0 closed;
`-1`, the default, follows `modality`: Fluorescence opens the lasers, BrightField the lamp). In Micro-Manager they
are the `Lasers` and `TransmittedLamp` shutter devices. Lasers alone give the fluorescence image, the lamp alone the
BrightField image. **Both open** add up on one camera: the fluorescence photons plus the lamp's photons times the
camera's QE at the lamp wavelength, then one noise chain (so the lamp's shot noise sits on the fluorescence, as it
would on a real scope). **None open** gives dark frames: offset, read noise and dark current only.

<!-- fig:light-shutters -->

## Illumination geometry

One number moves the laser spot across the objective's back focal plane (`illum-bfp`, MM `Lasers.BfpPosition`, the
shipped configs' `Illumination` group: Epi / HILO / TIRF): 0 is the centre (**epi**: the beam goes straight up), 1 the
critical angle, between them **HILO** (a highly inclined sheet, [[tokunaga2008](../references.md#tokunaga2008)]) and
above it **TIRF** (an evanescent layer over the coverslip,
[[axelrod1981](../references.md#axelrod1981)], [[axelrod2001](../references.md#axelrod2001)]), up to the pupil edge
NA / n of the medium (1.047 for the 1.40 objective in water, n = `bf-n-medium`, MM `SampleHolder.MediumIndex`, 1.337);
beyond it no light reaches the sample (MM `Lasers.IlluminationRegime` reads `Clipped`). The position is linear in the
BFP radius, so 0.5 is half-way to the critical angle.

The beam's cross-section is `illum-profile` (Flat, Gaussian, the default, or FlatTop: a disc with a soft edge) with
`illum-fwhm` (% of the FOV width, default 100: the default Gaussian gives the middle of each edge half the centre's
light and the corners a quarter). Each dye is excited by the laser power times a factor at its place:

- epi: the profile, the same at every height;
- HILO: the profile tilted into a sheet through the focal plane, about W / (tan θ<sub>s</sub> cos θ<sub>i</sub>)
  thick on the axis (W the beam's FWHM; at 0.95 about 0.6 W: 15 µm for the default beam over a 25.6 µm field, so
  narrow the beam, `illum-fwhm`, for a thin sheet; MM `Lasers.IlluminationDepthNm`), dyes above or below it unlit;
- TIRF: the profile at the coverslip times \(e^{-z/d}\), \(d = \lambda / (4\pi n \sqrt{u^2-1})\) (about 150 nm
  at 1.03 and 640 nm), with the interface's field enhancement (up to ~4x near the critical angle).

The factor changes the photons, and through the local light the kinetics: dSTORM switching, PALM activation and
bleaching run at the local excitation, each dye at the dose its own place and height has had
([Illumination history](photophysics.md#illumination-history-micro-manager-adapter); a cli or viewer movie's past is
`start-sec` at its own light), so a TIRF field bleaches its bottom layer and leaves the dyes higher up fresh.
The DNA-PAINT light presets set HILO (0.95), the nanorulers TIRF (1.03), the other presets epi (all *estimates*); with
several labelled targets (`light-preset` auto) the position their presets agree on, else epi.
Supercritical-angle emission and the beam's angular spread in the BFP are not modelled. The physics in full:
`spec/ALGORITHM.md`, "Excitation geometry".

The excitation factor a movie applies, in a section through the axis and on the axis:

<!-- fig:light-geometry -->

What it does to a frame, with the free DNA-PAINT imager's background:

<!-- fig:light-geometry-images -->

## The DNA-PAINT imager background

Unbound imager in the illuminated volume adds an offset per pixel and second of

\[ c\,N_A\,A_{px}\;k_{em}\eta F \int_0^H \int K_{|z - z_f|}(\mathbf r)\, e(\mathbf x + \mathbf r, z)\,d^2r\,dz, \]

with \(c\) the imager concentration, \(H\) the chamber height (`chamber-height-um`, default 100 µm, an *estimate*:
tape and flow chambers), \(A_{px}\) the pixel area in the sample, \(e\) the excitation factor of the illumination
geometry (`illum-bfp`) and \(K\) the spread of a dye's light at a distance \(|z - z_f|\) from the focal plane: the
light of the imager high above the focus lands on the camera defocused, spread over a disc of radius
\(|z - z_f| \tan\theta\), so a pixel collects the lit imager within that reach of it, not the column above it alone.
\(K\) is geometric optics in one medium: an isotropic emitter (a freely rotating imager), its emission angles weighted by
the Fresnel power transmittance into the coverslip (Born & Wolf 1999, section 1.5.3; s and p averaged), collected with
equal power per solid angle by an aplanatic objective (Richards & Wolf 1959) up to its NA. Without the weighting the
focal-plane irradiance of a dye at defocus \(\delta\) is \(\delta / 2\pi(\delta^2 + r^2)^{3/2}\), and on the axis of a
lit disc of radius \(a\) the column is \(\int_0^H 1 - \delta/\sqrt{a^2 + \delta^2}\,d\delta \approx a\) for
\(H \gg a\); the Fresnel weighting favours the steep rays, so the effective column is a little deeper.

So in epi the background is set by the **beam's width**, not the chamber's height: about 15 µm on the axis of the
default beam, a Gaussian of 25.6 µm FWHM (14 µm with a FlatTop that wide; 100 µm before this model), and about half
that at the beam's edge. In TIRF only the
evanescent layer is lit (about the field factor times the decay depth, ~0.5 µm), in HILO about the inclined sheet. That
is why DNA-PAINT is imaged in HILO or TIRF: the DNA-PAINT light presets set HILO (`illum-bfp` 0.95), the nanorulers TIRF
(1.03), both *estimates*; TIRF cuts the nanorulers' imager background about 13-fold against epi. The renderers evaluate
it on a node grid (a third of the beam's edge width apart) and interpolate: within 0.2 % of a dense quadrature in epi
and TIRF, 2 % in HILO. The map depends on the focus (the defocus of each height; HILO's sheet pivots at the focus), so
it is made at foci 0.25 µm apart and blended linearly between the two around the focus: a focus move reuses them, and
Micro-Manager makes the neighbouring ones ahead on a worker thread. A Flat beam (laterally infinite) lights every plane
alike, so the spread changes nothing: there the column is the plain integral, \(H\) in epi.

**Left out:** the index mismatch between the medium and the glass (the defocus is taken in one medium), the dye's
emission dipole (a free imager rotates: isotropic), the near-field (supercritical-angle) emission of dyes within a
wavelength of the coverslip, **imager depletion and its exclusion from the cells**: the background is the same over
cells and medium and does not drop as imager binds.

The background against the imager concentration, the model next to the rendered photons:

<!-- fig:light-imager -->

## Continuous populations: mean field or per dye

Pre states, the dSTORM initial ON and WideField-mode dyes emit continuously. Such a population renders either

- **mean field**: the structure's dye density binned on world-anchored z planes (`wf-plane-nm`) on a grid
  (`wf-upscale` cells per pixel, spanning the FOV plus a 2 µm margin), convolved with the state's PSF, times the exact
  mean photons per dye of the frame, \(r\,(e^{-\lambda t_0} - e^{-\lambda t_1})/\lambda\) (under a shaped beam with
  each place's own dose and rate); or
- **per dye**: each dye's window (its own address-based end: bleached at aux \(\times\) budget / \(k_{em}\), activated,
  or the initial ON's end) splatted as a unit image into a running image that changes only where windows start or end.

The switch is per frame: mean field while the expected emitters exceed `mean-field-density-per-um2` (default 20) in the
`mean-field-slab-nm` (500 nm) slab around focus, or `mean-field-max-emitters` (5000) in the z range; per dye below
(MM `Renderer.MeanField*`). Both give the same mean (checked within 1 % in total, a few % per pixel).

One population rendered both ways:

<!-- fig:light-meanfield -->

## Blinks: splat, binned or mean field

The blinks of dSTORM, PALM and DNA-PAINT render in one of three regimes, chosen per label and per frame:

| Regime | What is drawn | Exact | Lost |
|---|---|---|---|
| **SMLM** (splat) | every blink, its PSF splatted at its position | everything | -- |
| **Binned** (approximate SMLM) | the same blinks, each snapped to a grid of `blink-binned-upscale` cells per pixel, one density per PSF z plane, FFT-convolved with that plane's kernel | which dyes blink, their photons and z plane, the frame-to-frame fluctuation | the position within a cell (±half a cell, ±25 nm at the default 2 cells per 100 nm pixel) |
| **Mean field** | no blinks: each dye's *expected* ON time in the frame times the dye density, convolved once | the mean image | the blinking itself |

- **Splat or binned.** The splat costs per blink, the binned FFT per field-of-view area. A frame renders binned while
  more than `blink-binned-density-per-um2` (default 12.5; MM `Renderer.BlinkBinnedDensityPerUm2`) blinks are ON per µm² of the field of view, or
  `blink-binned-max-emitters` (1e9) in all; splatted below. The default is an *estimate*, measured on this
  implementation (the splat/binned break-even is ~10 per µm²; 12.5 sits just above it): 256 px × 100 frames of DNA-PAINT took 2.9 / 22.9 / 240 s splatted at 1 / 10 / 100 nM imager and
  13–20 s binned at any density (upscale 2; 3–4 s at upscale 1). Binned against splat, same blinks: the photons agree
  within 0.1 %, the mean image within 0.5–2 % (relative L2).
- **Mean field** is decided before any blink is drawn, from the expected ON emitters: above
  `blink-mean-field-density-per-um2` per µm² of the `mean-field-slab-nm` slab, or `blink-mean-field-max-emitters` in
  the z range. A movie that is mean-field throughout draws no blinks at all. The expected ON time is closed form:
  dSTORM and PALM follow the dye's chain (initial ON → first dark → ON → bleached | OFF → ON) as a matrix exponential
  through each piece of the light history; DNA-PAINT integrates its 1 s binding bins exactly. It matches the core's
  own blinks to within 0.2 % (some 10^5 to 10^7 dyes).
- **Mean field is off by default** (both thresholds 1e9), on purpose. For continuous emitters the mean image is exact
  up to shot noise. For blinks it is not: the blink fluctuation has a variance of about (photons per blink) times the
  shot-noise variance, and that fluctuation *is* the SMLM signal. A blink-mean-field movie has the right total signal
  (within 0.1 %), but its frame-to-frame noise is shot noise only (std 41 against 126 ADU in the check). Use it as a
  fast picture of where a dense label sits, not as SMLM data.
- The regime can change both ways within a movie (a power step, a 405 nm pulse). The progress line says which one drew
  a frame: `SMLM: N blinks (splat)`, `(binned FFT)`, or `blinks: mean-field (FFT)`.

The three regimes at a low, medium and high imager concentration:

<!-- fig:light-blink-regimes -->

## Where it lives

JS reference: `web/prototype/scope/spectra.js`, `dye_library.js`, `fluorescence.js`, `scope_movie.js`. C++:
`adapter/inSiliScope/Simulation/Spectra.*`, `LightPath.*`, `DyeLibrary.*` (data generated into `DyeLibraryData.inc` by
`tools/gen_dye_library.mjs`), `ScopeMovie.*` (`FluorescenceMovie`), `BinnedBlinks.*` and `BlinkExpectation.*` (JS `binned_blinks.js`,
`blink_expectation.js`). The core's label model (ABI 10) is in
`core/src/dyes.*`; spec/PORT.md section 16 has the details.
