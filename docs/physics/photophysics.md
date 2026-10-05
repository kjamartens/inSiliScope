# Photophysics

Every structure's label (issue 16) has a **mode** that says how its dyes switch: dSTORM, PALM, DNA-PAINT or WideField.
The rates come from the dye's data and the light path ([Dyes and light path](dyes-and-light-path.md)); this page is
about the switching itself. The third modality, **BrightField**, images transmitted light and has no photophysics: see
[Optics](optics.md#brightfield-imaging).

Each dye's schedule is a pure function of its address, so the same dye is the same dye every time the stage returns,
and a window query equals the union of its slices. Time is simulated time (frame \(\times\) exposure) since the
illumination came on; movies and the Micro-Manager stack and live mode start at **60 s** (`start-sec`), past the dSTORM
initial ON phase. Only blinks in the window a movie asks for are scheduled (the window \([t_0, 2t_1)\)), so a late
start costs nothing extra.

## Blinking dyes (dSTORM, PALM)

1. First activation at \(t_{act} = -\ln U / k_{act}\), with \(k_{act}\) the activation rate per dark dye (from the
   dye's off time and the 405 nm or primed-conversion light, see the light-path page).
2. Repeat: ON for \(\mathrm{Exp}(\tau_{on})\) (`on-sec`); then bleach with probability \(p_b\) (`bleach-prob`),
   otherwise dark for \(\mathrm{Exp}(\tau_{off})\) (`off-sec`) and blink again.
3. Per-blink brightness is log-normal with mean 1 and coefficient of variation `photon-cv` (0 = every blink equally
   bright).
4. At most 1000 blinks per dye; \(p_b\) is clamped to [0.01, 1].

**dSTORM** dyes start in an **initial ON** phase: every dye emits from \(t = 0\) for \(\mathrm{Exp}(\tau_{init})\)
(`initial-on-sec`, its own draw) before the blink schedule starts. The dSTORM times scale with the excitation rate so
photons per blink and duty cycle stay as measured [[dempsey2011](../references.md#dempsey2011)].

**PALM** proteins with a **pre state** (mEos3.2, Dendra2: green before photoconversion) emit in that state from \(t=0\)
until their first activation (and bleach in it at their pre photon budget). The pre state has its own spectrum, so it
is detected (and imaged with its own PSF) only as far as the light path lets it through.

## DNA-PAINT

Persistent sites never bleach. The imager binds at rate \(k_{on} c\) (`kon` x `mt-imager-nm`), a Poisson process for ever,
addressed per 1 s time bin (count, start times, ON time \(\mathrm{Exp}(\tau_{on})\) capped at \(20\tau_{on}\),
brightness), so any window is answered without running from \(t=0\). Overlapping binding events on one site are allowed
(fine while \(k_{on}c\,\tau_{on} \ll 1\)). The free imager adds a flat background (see the light-path page; its
depletion and its exclusion from cells are ignored).

## WideField

Every labelled dye emits from \(t=0\) and bleaches after an emitted-photon budget \(B\) (`photon-budget`, per dye
\(B \times\) an Exp(1) draw): the bleach rate is \(\lambda = k_{em}/B\), the half time \(\ln 2\,B/k_{em}\). A frame
holds the exact mean photons per dye, \(r\,(e^{-\lambda t_0} - e^{-\lambda t_1})/\lambda\) with \(r\) the detected rate,
rendered mean-field or per dye.

## Continuous populations

The dSTORM initial ON, PALM pre states and WideField dyes are **continuous populations**: rendered mean-field while
dense, per dye when sparse ([details](dyes-and-light-path.md#continuous-populations-mean-field-or-per-dye)).

## Photons

An ON dye emits its detected rate \(\times\) brightness photons per second, integrated over the part of the frame it
is on (frame-overlap weighting); the adapter's illumination profile (`Optics_IlluminationProfile`) multiplies it.
Bleaching is a function of time everywhere in the sample (the whole sample is illuminated from \(t=0\)); there is no
per-region bleach memory any more.
