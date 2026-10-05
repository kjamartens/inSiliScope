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

Noise draws are counter-based (`pcg4d`), so frames are independent and the CPU and GPU paths agree except for float32 rounding
(at least 99.8% of pixels identical; the rest differ by one electron in a Poisson draw). Two draws from one sequential stream
are never put in one C++ expression (evaluation order is unspecified); this once made the sCMOS noise non-reproducible across
rebuilds.

## Background

A flat background (`Background_BackgroundPhotonsPerSec`) multiplies an illumination field and fades with time:

- **fade**: \(0.3 + 0.7\,e^{-t/\tau}\) (`Background_DecaySec`);
- **illumination**: a peak-normalised profile (`FluoParam_IllumProfile`, `IllumFwhmPct`) multiplies background and emitters.

Out-of-focus light needs no extra population: every dye of the cell field sits at its own depth and is drawn with the
defocused PSF.

## Drift

A random drift direction per seed (its own RNG stream), speed `SimType_DriftNmPerSec`.
