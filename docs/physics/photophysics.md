# Photophysics

Two modalities share one dye population but use different photophysics:

- **SuperRes**: individual dyes switch on and off (a blink model, below).
- **WideField**: all labelled dyes emit continuously and bleach (a dose model, below).

## SuperRes: blinking

Each labelled dye has a schedule that is a pure function of its address, so the same dye is the same dye every time the
stage returns, and a window query equals the union of its slices.

**Bleaching dyes** (a finite life, generated once for all time):

1. First activation at \(t_{act} = -\ln U / k_{act}\), with \(k_{act}\) the activation rate per dark dye
   (`SimType_CellFieldMilliActivationRatePerDyePerSec`, default \(1.43\times10^{-3}\ \mathrm{s^{-1}}\)).
2. Repeat: ON for \(\mathrm{Exp}(\tau_{on})\) (`FluoParam_OnLifetimeSec`); then bleach with probability \(p_b\)
   (`FluoParam_BlinkBleachProb`), otherwise dark for \(\mathrm{Exp}(\tau_{off})\) (`FluoParam_OffLifetimeSec`) and blink again.
3. Per-blink brightness is log-normal with mean 1 and coefficient of variation `FluoParam_PhotonCV` (default 0.5; cli/viewer
   `photon-cv`; 0 = every blink equally bright).
4. At most 1000 blinks per dye (so a tiny \(p_b\) cannot loop forever); \(p_b\) is clamped to [0.01, 1].

**Persistent (DNA-PAINT-like) sites** never bleach. Their blinks are a Poisson process of rate \(k_{act}\) for ever, addressed
per 1 s time bin (count, start times, ON time \(\mathrm{Exp}(\tau_{on})\) capped at \(20\tau_{on}\), brightness), so any
window is answered without running from \(t=0\). Overlapping binding events on one site are allowed (fine while
\(k_{act}\tau_{on} \ll 1\)).

**Emitter density** (the adapter's `General_EmitterDensityPerSec` for non-CellField patterns) is the rate of blinks
switching ON per um\(^2\) per second, independent of exposure, ON lifetime and bleaching: the engine sets the steady-state ON
density to rate \(\times\) mean ON time. For the CellField pattern the blink rate comes from the dyes instead.

**Photons**: an ON dye emits `FluoParam_PhotonsPerSecond` \(\times\) brightness photons per second, integrated over the part of
the frame it is on (frame-overlap weighting). The illumination field multiplies it. Time is simulated time
(frame \(\times\) exposure), not wall-clock.

## WideField: dose model in physical units

Constants:

\[ \sigma = \frac{\ln 10\; 10^{3}\,\varepsilon}{N_A} \approx 3.8235\times10^{-13}\,\varepsilon\ \mathrm{um^2} \]

with \(\varepsilon\) the extinction coefficient (M\(^{-1}\)cm\(^{-1}\), default 270000). Emission rate per dye:

\[ k_{em} = \mathrm{QY}\;\sigma\;\Phi\, I(x,y) \]

with \(\Phi\) the excitation photon flux (um\(^{-2}\)s\(^{-1}\), default \(4\times10^{8}\), about 0.0125 W/cm\(^2\) at 640 nm: dim enough that shot noise shows), \(I\) the illumination pattern (peak 1)
and QY the quantum yield (0.7). Each bleaching dye has an emitted-photon budget \(B\) (default 5000; 0 = never bleaches). With
\(D\) the emitted-photon dose a dye has already produced, the surviving fraction is \(e^{-D/B}\).

Collected photons per frame use the collection efficiency of the objective,

\[ \eta = \tfrac12\left(1 - \sqrt{1 - (\mathrm{NA}/n)^2}\right), \]

and the **exact** frame integral of the bleaching decay, not an approximation:

- bleaching dyes: \(\; n_b\,\eta\,B\,e^{-D_0/B}\,(1 - e^{-\Delta D/B})\)
- persistent dyes: \(\; n_p\,\eta\,\Delta D\)

where \(\Delta D = k_{em}\Delta t\) is the dose per frame. The defaults give a half time \(t_{1/2} = 120\) s (reported by the
read-only property `FluoParam_WideFieldHalfTimeSec`, -1 = never) and about 0.45 photons per dye per 50 ms frame. QE is applied
later by the camera noise chain.

**Bleach memory.** In live mode a world-anchored `BleachField` stores the dose in sparse tiles on the dye grid: bleach a
region, move away and come back, and it is still dim. It is reset when the world or the grid pitch changes. Precomputed stacks
are a fresh sample (frame \(f\) starts at dose \(f\,\Delta D\)) and never touch the live map.

Persistent dyes never bleach, so with the default labelling (0% bleaching) nothing visibly bleaches in widefield.
