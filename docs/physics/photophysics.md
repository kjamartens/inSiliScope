# Photophysics

Every structure's label (issue 16) has a **mode** that says how its dyes switch: dSTORM, PALM, DNA-PAINT or WideField.
The rates come from the dye's data and the light path ([Dyes and light path](dyes-and-light-path.md)); this page is
about the switching itself. The third modality, **BrightField**, images transmitted light and has no photophysics: see
[Optics](optics.md#brightfield-imaging).

Each dye's schedule is a pure function of its address, so the same dye is the same dye every time the stage returns,
and a window query equals the union of its slices. Time is simulated time (frame \(\times\) exposure) since the
illumination came on. cli and viewer movies start at **60 s** (`start-sec`), past the dSTORM initial ON phase; the
Micro-Manager adapter reads each dye at its place's own clock (the illumination history, below). Only blinks in the window a movie asks for are scheduled (the window \([t_0, 2t_1)\)), so a late
start costs nothing extra.

What a few dyes of each mode emit over time, read from the core's schedules through the cli's dye output:

<!-- fig:photo-traces -->

## Blinking dyes (dSTORM, PALM)

!!! danger "Each blink is drawn with a cut PSF"
    Blinks are splatted without the pixels that would get less than 3e-6 of the emitter's photons (`Renderer.Quality`
    Realistic; Fast: 1e-5; Exhaustive: the whole kernel). Nothing is renormalized. Continuous populations keep the whole
    kernel. Details: [Optics, the halo cut](optics.md).


1. First activation at \(t_{act} = -\ln U / k_{act}\), with \(k_{act}\) the activation rate per dark dye (from the
   dye's off time and the 405 nm or primed-conversion light, see the light-path page).
2. Repeat: ON for \(\mathrm{Exp}(\tau_{on})\) (`on-sec`); then bleach with probability \(p_b\) (`bleach-prob`),
   otherwise dark for \(\mathrm{Exp}(\tau_{off})\) (`off-sec`) and blink again.
3. Per-blink brightness is log-normal with mean 1 and coefficient of variation `photon-cv` (0 = every blink equally
   bright).
4. At most 1000 blinks per dye; \(p_b\) is clamped to [0.01, 1].

The spread of the per-blink brightness on one field:

<!-- fig:photo-brightness -->

**dSTORM** dyes start in an **initial ON** phase: every dye emits from \(t = 0\) for \(\mathrm{Exp}(\tau_{init})\)
(`initial-on-sec`, its own draw) before the blink schedule starts. The dSTORM times scale with the excitation rate so
photons per blink and duty cycle stay as measured [[dempsey2011](../references.md#dempsey2011)].

The initial ON phase at the start of the illumination:

<!-- fig:photo-dstorm-start -->

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

Bleaching over ten minutes, the rendered photons against the model's half time:

<!-- fig:photo-wf-bleach -->

## Continuous populations

The dSTORM initial ON, PALM pre states and WideField dyes are **continuous populations**: rendered mean-field while
dense, per dye when sparse ([details](dyes-and-light-path.md#continuous-populations-mean-field-or-per-dye)).

## Photons

An ON dye emits its detected rate \(\times\) brightness photons per second, integrated over the part of the frame it
is on (frame-overlap weighting); the adapter's illumination profile (`Lasers.IlluminationProfile`) multiplies it.
In a cli or viewer movie the whole sample has been lit since \(t=0\).

## Illumination history (Micro-Manager adapter)

The adapter remembers how long each place has been lit: seconds of illumination per 0.25 um tile of the world, weighted
by the illumination profile (in 1/16 steps). Every dye's schedule is read at its tile's clock, so imaging bleaches,
photoconverts and uses up dyes only where the light fell: bleach a region, move away and come back, and it is still
dim; a place never lit starts at clock 0 (dSTORM dyes in their initial ON phase, PALM proteins unconverted, WideField
dyes unbleached). A live frame lights the FOV and its 2 um margin for one exposure when it is taken (a snap or a
sequence acquisition; an idle live loop lights nothing). A snap takes a frame started after it was called, a sequence
acquisition frames started after it began, so the first frame after a stage move shows the new place and each snap's
clocks include the light of the one before; a stack reads the history and adds its whole duration (so a
stack is reproducible only on a freshly loaded device). Clocks are in seconds at the light path's current settings: a
later change of laser power does not rescale the time already accumulated. The history is cleared when the world
changes (seed, cell parameters). Precision: tile 0.25 um; the lit rect is the stage pose's, drift is not followed.
