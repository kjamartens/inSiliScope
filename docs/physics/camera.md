# Camera and background

## Noise chain

Per pixel, in this order:

1. quantum efficiency (`CamParam_QuantumEfficiency`, default 0.85),
2. plus dark current (`CamParam_DarkCurrentElectronsPerSec`, 1.03 e-/s),
3. Poisson shot noise,
4. Gaussian read noise (`CamParam_ReadNoiseElectrons`, 1.2 e-), optionally with a per-pixel spread
   (`CamParam_ReadNoiseStdPctPerPixel`, 20%),
5. gain (`CamParam_GainPhotonsPerADU`, 0.25 ADU/e-; per-pixel spread `CamParam_GainStdPctPerPixel`, 0.5%),
6. a static per-pixel offset (`CamParam_OffsetADU` 100, std `CamParam_OffsetStdADU` 0.5),
7. 16-bit clamp.

Defaults follow the Photometrics Kinetix22 sCMOS (Sensitivity mode) datasheet; the per-pixel spreads are estimates because
vendors do not publish them. The gain spread (PRNU, a static pattern proportional to the signal) is 0.5%, typical of
sCMOS; it was 5% until 2026-10-01, which hid little in single-molecule frames but swamped brightfield contrast. An **EMCCD** path (`CamParam_CameraType`) adds EM gain and clock-induced charge but keeps this
project's dark current.

The gain (`CamParam_GainPhotonsPerADU`, e⁻/ADU) is the whole conversion from photoelectrons to counts for both sensor
types. The EM gain does not scale the signal: the multiplication is drawn as Gamma(shape = n, scale = 1) on the n
photoelectrons, so its mean stays n and it adds the \(\sqrt{2}\) excess noise factor; the EM gain divides the read noise
(50 e⁻ at an EM gain of 150 is 0.33 e⁻ effective). The EM gain is not set on its own: it is the camera preset's
pre-amplifier sensitivity (e⁻/ADU after the EM register; 1 e⁻/ADU for the iXon, an *estimate*, and when a preset has
none) divided by the gain (Micro-Manager's `CamParam_EmGain` is read-only; the cli's `em-gain` can override it). The iXon
Ultra 897 preset sets the gain per photoelectron: 0.0066 e⁻/ADU (about 150 ADU per photoelectron, EM gain about 150)
for dSTORM, PALM and DNA-PAINT, 0.1 e⁻/ADU (EM gain 10) for WideField and BrightField, where 0.0066 would saturate the
16-bit output (*estimates*). A mode or modality change re-applies it (viewer and Micro-Manager); the cli picks it from
the spec's mode and modality unless `gain` is given.

Noise draws are counter-based (`pcg4d`), so frames are independent and the CPU and GPU paths agree except for float32 rounding
(at least 99.8% of pixels identical; the rest differ by one electron in a Poisson draw). Two draws from one sequential stream
are never put in one C++ expression (evaluation order is unspecified); this once made the sCMOS noise non-reproducible across
rebuilds.

## Background

A flat background (`Background_BackgroundPhotonsPerSec`) multiplies an illumination field and fades with time:

- **fade**: \(0.3 + 0.7\,e^{-t/\tau}\) (`Background_DecaySec`);
- **illumination** (Micro-Manager adapter): a peak-normalised profile (`Optics_IlluminationProfile`,
  `Optics_IlluminationFwhmPct`) multiplies background and blinks.

For fluorescence the background is in detected photons: it is multiplied by the camera's QE at the emission filter's
centre, and the noise chain runs at QE 1 because each dye's detected fraction already holds the QE curve
([Dyes and light path](dyes-and-light-path.md)). Camera presets (`camera-preset`, MM `CamParam_CameraPreset`:
Kinetix22, iXon Ultra 897, Custom) set the noise values and the QE curve (`qe-curve`, MM `CamParam_QeCurve`;
`Custom` = flat at `qe`). BrightField uses the curve's QE at its lamp wavelength.

Out-of-focus light needs no extra population: every dye of the cell field sits at its own depth and is drawn with the
defocused PSF.

## Drift

The sample drift has two parts, set separately for xy and z: a **directed** part (a slow, mostly steady movement in
one direction, as from thermal expansion or a creeping stage) and a **random walk** on top.

**Directed part.** A mean velocity: xy speed \(V_{xy}\) (`SimType_DriftXySpeedNmPerSec`, cli/viewer
`drift-xy-speed-nm-per-sec`) in a direction \(\theta_0\) (`SimType_DriftXyAngleDeg`, degrees from +x; −1, the default,
draws it once per seed) and a signed z speed \(V_z\) (`SimType_DriftZSpeedNmPerSec`, + = away from the coverslip).
Direction and strength wander slowly:

\[
v_{xy} = V_{xy}\,\max(0, 1 + w\,s_{xy})\;(\cos(\theta_0 + \alpha\,\phi),\ \sin(\theta_0 + \alpha\,\phi)), \qquad
v_z = V_z\,\max(0, 1 + w\,s_z),
\]

where \(\phi, s_{xy}, s_z\) are independent unit-variance Ornstein–Uhlenbeck processes with correlation time \(\tau\)
(`SimType_DriftWanderTimeSec`, default 60 s), \(\alpha\) the direction wander (`SimType_DriftXyAngleWanderDeg`, deg RMS)
and \(w\) the speed wander (`SimType_DriftSpeedWanderPct`, % RMS of the mean). So the xy direction strays by about
\(\alpha\) around \(\theta_0\) and the z drift keeps its sign while its strength fluctuates. With both wanders at 0 the
velocity is constant. Each frame moves the sample by \(v\,\Delta t\), with \(v\) at the frame's start. The xy and z
speeds are the everyday settings; direction, wanders and \(\tau\) are advanced (the viewer shows them under Advanced).

**Random walk.** Every frame adds an independent normal step per axis,
and the steps add up (the "cumulative normal distribution" of Cnossen et al. 2021, used by Ma et al. 2024 at RMS drifts of
5, 10 and 20 nm/s). The step of a frame of length \(\Delta t\) has variance \(\sigma^2 \Delta t\) per axis, so

\[
d(0) = 0, \qquad d(f) = d(f-1) + \sigma \sqrt{\Delta t}\; g_f, \qquad \langle d(t)^2 \rangle = \sigma^2 t ,
\]

and \(\sigma\) is the RMS displacement per axis after 1 s whatever the frame rate (strictly nm/\(\sqrt{\text{s}}\); "nm/s" in
the papers' wording; advanced, "jitter" in the viewer). x and y each get \(\sigma_{xy}\) (`SimType_DriftXyNmPerSqrtSec`, cli/viewer
`drift-xy-nm-per-sqrt-sec`), z gets \(\sigma_z\) (`SimType_DriftZNmPerSqrtSec`, `drift-z-nm-per-sqrt-sec`); both default
to 0. A stable setup (optical table, active isolation, constant temperature) drifts a few nm/s.

- The draws are counter-based per (seed, frame) on a stream of their own, so a precomputed stack, live mode, the cli, the
  viewer and webSMLM's `CellField.driftTrajectory` follow the same path for one seed, and no other draw moves. The drift
  starts at zero at each acquisition (the movie's first frame; every Live/MDA start in Micro-Manager) and is constant
  within a frame (motion during the exposure is ignored).
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
