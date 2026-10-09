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
| DNA-PAINT | persistent binding sites: imager binds at \(k_{on} c\), never bleaches | blinks, plus the free imager's background |
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
with \(c\) the imager concentration (`mt-imager-nm`, MM `CellField.Microtubules_ImagerNm`).

## Light-path presets

`data/dyes/light_path.json` defines the lasers (405, 488, 561, 640, 730 nm and a custom line), dichroics, emission
filters and presets: `dSTORM-640/561/488` (Dempsey's conditions [[dempsey2011](../references.md#dempsey2011)]),
`PAINT-640/561/488`, `PALM-561`, `PALM-primed`, `WF-405/488/561/640`. Intensities without a source are *estimates*.
Every dye mode names its preset; `light-preset=auto` (cli/viewer) or a dye/mode change (viewer, MM `Lasers.Preset`)
applies it, setting every laser, the dichroic and the filter the spec does not give. The default is `PAINT-640` (640 nm
at 1 kW/cm\(^2\), an *estimate*; LP650, 676/37) for the default ATTO 655 DNA-PAINT label. Camera presets
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

## The DNA-PAINT imager background

Unbound imager in the illuminated volume adds a flat offset per pixel and second of

\[ c\,N_A\,H\,A_{px}\;k_{em}\eta F, \]

with \(c\) the imager concentration, \(H\) the illuminated chamber height (`chamber-height-um`, default 5 µm, an
*estimate*: the `Epi` geometry illuminates the whole chamber) and \(A_{px}\) the pixel area in the sample. **Imager
depletion and its exclusion from the cells are ignored**: the background is the same over cells and medium and does not drop as imager
binds.

The background against the imager concentration, the model next to the rendered photons:

<!-- fig:light-imager -->

## Continuous populations: mean field or per dye

Pre states, the dSTORM initial ON and WideField-mode dyes emit continuously. Such a population renders either

- **mean field**: the structure's dye density binned on world-anchored z planes (`wf-plane-nm`) on a grid
  (`wf-upscale` cells per pixel, spanning the FOV plus a 2 µm margin), convolved with the state's PSF, times the exact
  mean photons per dye of the frame, \(r\,(e^{-\lambda t_0} - e^{-\lambda t_1})/\lambda\); or
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
  more than `blink-binned-density-per-um2` (default 10) blinks are ON per µm² of the field of view, or
  `blink-binned-max-emitters` (1e9) in all; splatted below. The default is an *estimate*, measured on this
  implementation: 256 px × 100 frames of DNA-PAINT took 2.9 / 22.9 / 240 s splatted at 1 / 10 / 100 nM imager and
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

## Where it lives

JS reference: `web/prototype/scope/spectra.js`, `dye_library.js`, `fluorescence.js`, `scope_movie.js`. C++:
`adapter/inSiliScope/Simulation/Spectra.*`, `LightPath.*`, `DyeLibrary.*` (data generated into `DyeLibraryData.inc` by
`tools/gen_dye_library.mjs`), `ScopeMovie.*` (`FluorescenceMovie`), `BinnedBlinks.*` and `BlinkExpectation.*` (JS `binned_blinks.js`,
`blink_expectation.js`). The core's label model (ABI 10) is in
`core/src/dyes.*`; spec/PORT.md section 16 has the details.
