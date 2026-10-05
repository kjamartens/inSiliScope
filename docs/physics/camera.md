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

A random drift direction per seed (its own RNG stream), speed `SimType_DriftNmPerSec`.
