# Camera and background

## Noise chain

Per pixel, in this order:

1. quantum efficiency (`Camera.QuantumEfficiency`, default 0.85, an *estimate*; the Kinetix22's peak QE is 95 %
   [[kinetix-datasheet](../references.md#kinetix-datasheet)]),
2. plus dark current (`Camera.DarkCurrentElectronsPerSec`, 1.03 e-/s
   [[kinetix-datasheet](../references.md#kinetix-datasheet)]),
3. Poisson shot noise,
4. Gaussian read noise (`Camera.ReadNoiseElectrons`, 1.2 e- [[kinetix-datasheet](../references.md#kinetix-datasheet)]),
   optionally with a per-pixel spread (`Camera.sCMOS_ReadNoiseStdPctPerPixel`, 20%, an *estimate*),
5. gain (`Camera.GainElectronsPerADU`, 0.25 e⁻/ADU [[kinetix-datasheet](../references.md#kinetix-datasheet)]; per-pixel
   spread `Camera.sCMOS_GainStdPctPerPixel`, 0.5%, an *estimate*),
6. a static per-pixel offset (`Camera.OffsetADU` 100, std `Camera.OffsetStdADU` 0.5; both *estimates*),
7. 16-bit clamp.

Read noise, dark current and gain follow the Photometrics Kinetix22 sCMOS (Sensitivity mode) datasheet
[[kinetix-datasheet](../references.md#kinetix-datasheet)]; the QE, the offset and the per-pixel spreads are *estimates*
because the datasheet does not give them. Every sCMOS pixel has its own offset, read noise and gain
[[huang2013](../references.md#huang2013)]. The gain spread (PRNU, a static pattern proportional to the signal) is 0.5%
(an *estimate*; a published sCMOS PRNU is 0.06 % rms at 15 000 e- and 0.3 % rms at 700 e-
[[orcaflash4v3-technote](../references.md#orcaflash4v3-technote)]); it was 5% until 2026-10-01, which hid little in
single-molecule frames but swamped brightfield contrast. An **EMCCD** path (`Camera.CameraType`) adds EM gain and
clock-induced charge but keeps this project's dark current. The per-pixel gain and read-noise spreads are an sCMOS's (an
amplifier per pixel): an EMCCD reads every pixel through one amplifier, so it ignores them (since 2026-10-06; the
per-pixel offset applies to both).

The chain switched on step by step, ending with both camera presets:

<!-- fig:cam-chain -->

The gain spread on a BrightField image, where it matters:

<!-- fig:cam-prnu -->

The gain (`Camera.GainElectronsPerADU`, e⁻/ADU) is the whole conversion from photoelectrons to counts for both sensor
types. The EM gain does not scale the signal: the multiplication is drawn as Gamma(shape = n, scale = 1) on the n
photoelectrons, so its mean stays n and it adds the \(\sqrt{2}\) excess noise factor
[[hirsch2013](../references.md#hirsch2013)]; the EM gain divides the read noise (50 e⁻ at an EM gain of 150 is 0.33 e⁻
effective; the iXon preset's 50 e⁻ is an *estimate*, the 897's specification gives 37-89 e⁻ at 5-17 MHz
[[ixon897-datasheet](../references.md#ixon897-datasheet)]). The EM gain is not set on its own: it is the camera preset's
pre-amplifier sensitivity (e⁻/ADU after the EM register; 1 e⁻/ADU for the iXon, an *estimate*, and when a preset has
none) divided by the gain (Micro-Manager's `Camera.EMCCD_EmGain` is read-only; the cli's `em-gain` can override it). The iXon
Ultra 897 preset sets the gain per photoelectron: 0.0066 e⁻/ADU (about 150 ADU per photoelectron, EM gain about 150)
for dSTORM, PALM and DNA-PAINT, 0.1 e⁻/ADU (EM gain 10) for WideField and BrightField, where 0.0066 would saturate the
16-bit output (*estimates*). A mode or modality change re-applies it (viewer and Micro-Manager); the cli picks it from
the spec's mode and modality unless `gain` is given.

The per-pixel maps (offset, gain and read-noise spreads) are a pure function of the seed and the camera settings, so
the same camera keeps the same fixed pattern in live mode and in a stack, whatever else changes (since 2026-10-08; live
mode redrew them on every property change before).

The gain and the EMCCD's excess noise, measured as photon transfer curves on the cli's frames:

<!-- fig:cam-ptc -->

The two presets and an ideal camera on one frame:

<!-- fig:cam-presets -->

Noise draws are counter-based (`pcg4d`), so frames are independent and the CPU and GPU paths agree except for float32 rounding
(at least 99.8% of pixels identical; the rest differ by one electron in a Poisson draw). Two draws from one sequential stream
are never put in one C++ expression (evaluation order is unspecified); this once made the sCMOS noise non-reproducible across
rebuilds.

## Background

A flat background (`SampleHolder.BackgroundPhotonsPerSec`) multiplies an illumination field and fades with time:

- **fade**: \(0.3 + 0.7\,e^{-t/\tau}\) (`SampleHolder.BackgroundDecaySec`; the form and its constants are *estimates*);
  \(t\) is the movie's time, in
  Micro-Manager the lit clock at the FOV centre ([Illumination history](photophysics.md#illumination-history-micro-manager-adapter):
  one value per frame, the fade does not vary across the FOV);
- **illumination** (Micro-Manager adapter): a peak-normalised profile (`Lasers.IlluminationProfile`,
  `Lasers.IlluminationFwhmPct`) multiplies background and blinks.

For fluorescence the background is in detected photons: it is multiplied by the camera's QE at the emission filter's
centre, and the noise chain runs at QE 1 because each dye's detected fraction already holds the QE curve
([Dyes and light path](dyes-and-light-path.md)). Camera presets (`camera-preset`, MM `Camera.CameraPreset`:
Kinetix22, iXon Ultra 897, Custom) set the noise values and the QE curve (`qe-curve`, MM `Camera.QeCurve`;
`Custom` = flat at `qe`). BrightField uses the curve's QE at its lamp wavelength.

Out-of-focus light needs no extra population: every dye of the cell field sits at its own depth and is drawn with the
defocused PSF.

## Drift

The sample drift has two parts, set separately for xy and z: a **directed** part (a slow, mostly steady movement in
one direction, as from thermal expansion or a creeping stage) and a **random walk** on top.

**Preset.** `SampleHolder.DriftPreset` (a Basic property; the viewer's **Drift** select, the `Drift` group of the
shipped configurations) sets both speeds and both random walks at once; the directions and wanders keep their values.
The tiers are *estimates* (Ma et al. 2024 simulate random walks of 5, 10 and 20 nm/\(\sqrt{\text{s}}\)
[[ma2024](../references.md#ma2024)], from High to four times High):

| Preset | \(V_{xy}\), \(V_z\) (nm/s) | \(\sigma_{xy}\), \(\sigma_z\) (nm/\(\sqrt{\text{s}}\)) |
|---|---|---|
| Off (default) | 0 | 0 |
| Low | 2 | 0.4 |
| Medium | 5 | 1 |
| High | 25 | 5 |
| Extreme | 250 | 50 |

Setting any of the four by hand makes the preset `Custom`. The speeds go up to 1000 nm/s, the walks to
200 nm/\(\sqrt{\text{s}}\) (4x Extreme).

The Extreme preset as a movie, and every preset's drift over 30 s:

<!-- fig:drift-presets -->

**Directed part.** A mean velocity: xy speed \(V_{xy}\) (`SampleHolder.DriftXySpeedNmPerSec`, cli/viewer
`drift-xy-speed-nm-per-sec`) in a direction \(\theta_0\) (`SampleHolder.DriftXyAngleDeg`, degrees from +x; −1, the default,
draws it once per seed), and a z speed \(V_z \ge 0\) (`SampleHolder.DriftZSpeedNmPerSec`) in a direction \(u = \pm 1\)
(`SampleHolder.DriftZDirection`: `Up` = away from the coverslip, `Down`, or `Random`, the default, drawn once per seed;
cli/viewer `drift-z-direction` 1 / −1 / 0). Direction and strength wander slowly, each within bounds:

\[
v_{xy} = V_{xy}\,\max(0, 1 + w\,s_{xy})\;(\cos(\theta_0 + \alpha\,S(\phi)),\ \sin(\theta_0 + \alpha\,S(\phi))), \qquad
v_z = u\,V_z\,\max(0, 1 + w\,s_z)\,\cos(\beta\,S(\psi)),
\]

where \(\phi, \psi, s_{xy}, s_z\) are independent unit-variance Ornstein–Uhlenbeck processes with correlation time
\(\tau\) (`SampleHolder.DriftWanderTimeSec`, default 60 s, an *estimate*) and \(S(x) = \operatorname{erf}(x/\sqrt 2)\)
maps each to a uniform value in (−1, 1) (A&S 7.1.26 [[abramowitz1964](../references.md#abramowitz1964)]), so the swings
stay within their bounds:

- \(\alpha\) (`SampleHolder.DriftXyAngleWanderDeg`, default 180, an *estimate*): the xy direction swings within
  \(\pm\alpha\) of \(\theta_0\), RMS \(\alpha/\sqrt 3\); 180 = it can turn to any direction.
- \(\beta\) (`SampleHolder.DriftZAngleWanderDeg`, default 90, an *estimate*, at most 180): the z drift is the speed
  times the cosine of a swing within \(\pm\beta\). At 90 it moves between full speed in its direction and standing
  still, never back; at 180 it also reverses for a while; 0 = steady.
- \(w\) (`SampleHolder.DriftSpeedWanderPct`): the strengths' fluctuation, % RMS of the mean.

With \(\alpha = \beta = w = 0\) the velocity is constant. Each frame moves the sample by \(v\,\Delta t\), with \(v\)
at the frame's start. The preset (or the two speeds) is the everyday setting; directions, wanders and \(\tau\) are
advanced (the viewer shows them under Advanced, MM at `Detail` Expert).

**Random walk.** Every frame adds an independent normal step per axis, and the steps add up (the "cumulative normal
distribution" of Cnossen et al. 2021 [[cnossen2021](../references.md#cnossen2021)], used by Ma et al. 2024 at RMS drifts
of 5, 10 and 20 nm/s [[ma2024](../references.md#ma2024)]). The step of a frame of length \(\Delta t\) has variance
\(\sigma^2 \Delta t\) per axis, so

\[
d(0) = 0, \qquad d(f) = d(f-1) + \sigma \sqrt{\Delta t}\; g_f, \qquad \langle d(t)^2 \rangle = \sigma^2 t ,
\]

and \(\sigma\) is the RMS displacement per axis after 1 s whatever the frame rate (strictly nm/\(\sqrt{\text{s}}\); "nm/s" in
the papers' wording; advanced, "jitter" in the viewer). x and y each get \(\sigma_{xy}\) (`SampleHolder.DriftXyNmPerSqrtSec`, cli/viewer
`drift-xy-nm-per-sqrt-sec`), z gets \(\sigma_z\) (`SampleHolder.DriftZNmPerSqrtSec`, `drift-z-nm-per-sqrt-sec`); both default
to 0. A stable setup (optical table, active isolation, constant temperature) drifts several nm/s
[[ma2024](../references.md#ma2024)].

- The draws are counter-based per (seed, frame) on a stream of their own, so a precomputed stack, live mode, the cli, the
  viewer and webSMLM's `CellField.driftTrajectory` follow the same path for one seed, and no other draw moves. A movie's
  (and a precomputed stack's) drift starts at zero at its first frame and is constant within a frame (motion during the
  exposure is ignored).
- In Micro-Manager's live mode the drift is the sample's: it starts at zero when the world is made (device load, seed or
  cell parameters) and **continues** across Live/MDA stops and starts (since 2026-10-08; it restarted at zero before).
  The first acquisition on a fresh sample follows the seed's path step by step. Between acquisitions
  `SampleHolder.TimeWhileIdle` decides: `Running` (default) keeps the sample drifting at the wall-clock rate (steps of at
  most 10 s), `Paused` holds it still. Switching the drift off keeps the sample where it is. `Camera.Test_DriftNm` (Test
  tier) reports the drift of the last frame taken.
- The drift is the sample's displacement in camera axes, +z away from the coverslip: the focal plane sits \(d_z\) lower
  in the sample.
- **Blinks and per-dye emitters** (dSTORM, PALM, DNA-PAINT; WideField labels drawn per dye): every emitter is drawn at
  its position plus the drift, with the focal plane moved by \(-d_z\).
- **Mean-field images (WideField labels, PALM pre states, the dSTORM initial ON) and BrightField**: under uniform illumination the image of a moved sample is the image moved. A frame is
  the scene's full-grid image spectrum (periodic and band-limited: grid pitch \(\le \lambda/4\mathrm{NA}\) resp.
  \(\lambda/4n\)) times a phase ramp, cropped and binned, so a sub-pixel drift is exact rather than re-binned. The
  illuminated square and the dye grid (mean field) or the grid margin (BrightField) grow by the xy drift. The z drift
  uses images at foci 10 nm apart, linearly interpolated (mean field: 2e-5 rms, BrightField: 2e-4 of the contrast
  against the exact focus). In Micro-Manager's live mode both shift the image of a scene anchored within 1 µm of
  the drifted sample (rebuilt when it moves further).
- The flat background and the DNA-PAINT imager background are unaffected.
- The cli writes the true drift per frame next to the movie (`<name>.drift.csv`: frame, dx, dy, dz in nm), ground
  truth for testing drift correction.

References ([cnossen2021](../references.md#cnossen2021), [ma2024](../references.md#ma2024)): J. Cnossen, T. J. Cui, C. Joo, C. Smith, "Drift correction in localization microscopy using entropy
minimization", *Opt. Express* **29**(18), 27961-27974 (2021), doi:10.1364/OE.426620. H. Ma, M. Chen, P. Nguyen, Y. Liu,
"Toward drift-free high-throughput nanoscopy through adaptive intersection maximization", *Sci. Adv.* **10**(21),
eadm7765 (2024), doi:10.1126/sciadv.adm7765.
