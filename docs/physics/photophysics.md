# Photophysics

Every structure's label (issue 16) has a **mode** that says how its dyes switch: dSTORM, PALM, DNA-PAINT or WideField.
The rates come from the dye's data and the light path ([Dyes and light path](dyes-and-light-path.md)); this page is
about the switching itself. The third modality, **BrightField**, images transmitted light and has no photophysics: see
[Optics](optics.md#brightfield-imaging).

Each dye's schedule is a pure function of its address, so the same dye is the same dye every time the stage returns,
and a window query equals the union of its slices. Time is simulated time (frame \(\times\) exposure) since the
illumination came on. cli and viewer movies start at **60 s** (`start-sec`), past the dSTORM initial ON phase; the
Micro-Manager adapter reads each dye at the light its own place has had (the illumination history, below). Only blinks in the window a movie asks for are scheduled (the window \([t_0, 2t_1)\)), so a late
start costs nothing extra.

What a few dyes of each mode emit over time, read from the core's schedules through the cli's dye output:

<!-- fig:photo-traces -->

## Blinking dyes (dSTORM, PALM)

!!! danger "Each blink is drawn with a cut PSF"
    Blinks are splatted without the pixels that would get less than 3e-6 of the emitter's photons (`Renderer.Quality`
    Realistic; Fast: 1e-5; Exhaustive: the whole kernel), and a blink that gives a frame fewer than 50 detected photons
    (Fast: 100; Exhaustive: 0) is not drawn in that frame. Nothing is renormalized. Continuous populations keep the whole
    kernel and every dye. Details: [Optics, the halo cut](optics.md).


1. First activation at \(t_{act} = -\ln U / k_{act}\), with \(k_{act}\) the activation rate per dark dye (from the
   dye's off time and the 405 nm or primed-conversion light, see the light-path page).
2. Repeat: ON for \(\mathrm{Exp}(\tau_{on})\) (`on-sec`); then bleach with probability \(p_b\) (`bleach-prob`),
   otherwise dark for \(\mathrm{Exp}(\tau_{off})\) (`off-sec`) and blink again. For dSTORM dyes the three follow from
   the measured photons per switching cycle, duty cycle and switching cycles (\(\tau_{on}\) = photons / detected rate,
   duty cycle = \(\tau_{on}/(\tau_{on}+\tau_{off})\), \(p_b\) = 1/cycles) [[dempsey2011](../references.md#dempsey2011)];
   the PALM values are *estimates*.
3. Per-blink brightness is log-normal with mean 1 and coefficient of variation `photon-cv` (0 = every blink equally
   bright; the log-normal shape and the library's 0.5 are *estimates*).
4. At most 1000 blinks per dye; \(p_b\) is clamped to [0.01, 1].

The spread of the per-blink brightness on one field:

<!-- fig:photo-brightness -->

**dSTORM** dyes start in an **initial ON** phase: every dye emits from \(t = 0\) for \(\mathrm{Exp}(\tau_{init})\)
(`initial-on-sec`, its own draw; 2 s at the reference intensity in the library, an *estimate*) before the blink
schedule starts. The dSTORM times scale with the excitation rate so photons per blink and duty cycle stay as measured
[[dempsey2011](../references.md#dempsey2011)].

The initial ON phase at the start of the illumination:

<!-- fig:photo-dstorm-start -->

**PALM** proteins with a **pre state** (mEos3.2 [[zhang2012](../references.md#zhang2012)], Dendra2
[[gurskaya2006](../references.md#gurskaya2006)]: green before photoconversion) emit in that state from \(t=0\) until
their first activation (and bleach in it at their pre photon budget, 2e5 in the library, an *estimate*). The pre state
has its own spectrum, so it is detected (and imaged with its own PSF) only as far as the light path lets it through.

## DNA-PAINT

Persistent sites never bleach. The imager binds at rate \(k_{on} c\)
[[jungmann2010](../references.md#jungmann2010), [schnitzbauer2017](../references.md#schnitzbauer2017)] and stays bound
for \(\mathrm{Exp}(\tau_{on})\). \(k_{on}\) and \(\tau_{on}\) belong to the docking and imager sequences, so they are
the **target's**, not the dye's (the dye sets the photons): `data/specimens.json` `paint`, options `<prefix>-paint-kon`
and `<prefix>-paint-on-sec` (Micro-Manager `<Target>_KonPerMolarPerSec`, `<Target>_BoundTimeSec`, Advanced), with the
imager concentration \(c\) `<prefix>-imager-nm`. Defaults (*estimates*):

| Target | \(k_{on}\) (M⁻¹s⁻¹) | \(\tau_{on}\) mean | half-time |
|---|---|---|---|
| Microtubules (`mt-*`) | 10⁶ | 0.05 s | 35 ms |
| Nuclear pores (`npc-*`) | 10⁶ | 0.05 s | 35 ms |
| Nuclear lamina (`lamin-*`) | 10⁶ | 0.05 s | 35 ms |
| Nanoruler docking strands (`ruler-*`) | 10⁶ | 0.289 s | 200 ms |

9-10 nt imagers bind with \(k_{on}\) of this order and 9 nt docking strands for about 0.3-1 s; the microtubules' 0.05 s
keeps their earlier look, and the pores and the lamina share it. The binding is a Poisson
process for ever, addressed per 1 s time bin (count, start times, ON time \(\mathrm{Exp}(\tau_{on})\) capped at
\(20\tau_{on}\), brightness), so any window is answered without running from \(t=0\). Overlapping binding events on one
site are allowed (fine while \(k_{on}c\,\tau_{on} \ll 1\)). The free imager adds a flat background (see the light-path
page; its depletion and its exclusion from cells are ignored).

## WideField

Every labelled dye emits from \(t=0\) and bleaches after an emitted-photon budget \(B\) (`photon-budget`, per dye
\(B \times\) an Exp(1) draw, i.e. a constant bleaching probability per emitted photon; the library's \(B = 10^5\) is an
*estimate*): the bleach rate is \(\lambda = k_{em}/B\), the half time \(\ln 2\,B/k_{em}\). A frame
holds the exact mean photons per dye, \(r\,(e^{-\lambda t_0} - e^{-\lambda t_1})/\lambda\) with \(r\) the detected rate,
rendered mean-field or per dye.

Bleaching over 200 s, the rendered photons against the model's half time:

<!-- fig:photo-wf-bleach -->

## Continuous populations

The dSTORM initial ON, PALM pre states and WideField dyes are **continuous populations**: rendered mean-field while
dense, per dye when sparse ([details](dyes-and-light-path.md#continuous-populations-mean-field-or-per-dye)).
Blinks are splatted one by one, or above 12.5 ON per µm² binned on a sub-pixel grid and FFT-convolved (approximate
SMLM); a mean-field regime without blinking exists but is off by default
([details](dyes-and-light-path.md#blinks-splat-binned-or-mean-field)).

## Photons

An ON dye emits its detected rate \(\times\) brightness photons per second, integrated over the part of the frame it
is on (frame-overlap weighting), times the excitation factor at its place (the beam's profile and, in HILO and TIRF,
its height: [Illumination geometry](dyes-and-light-path.md#illumination-geometry)).
In a cli or viewer movie the whole sample has been lit since \(t=0\).

## Illumination history (Micro-Manager adapter)

The adapter remembers what light every place of the sample has had. The sample's time is cut into records of one
light (its kind, its power, where the beam sat), and each record is added, at the beam's exact place, to maps of the
dose per place. Every dye is read at the dose at its own place and height, smooth in all three directions: the bleached
area has the beam's soft edge, after TIRF the dyes higher in a cell are still fresh, and a HILO sheet bleaches its own
inclined slab. This holds for every mode and however the dyes are drawn (blinks, a mean field, dye by dye). It replaced
(2026-10-16) a history that counted the beam in two levels per 0.25 µm tile and 12 height bands, with which blinking
and sparse populations stepped at the beam's half contour. Each kind of light keeps the light path and label settings
in force then (the laser lines' power ratios, excitation filter, dichroic, imager concentration, the dye's kinetic
overrides), and every dye's schedule is read through that **rate history** with the same random draws whatever the
history, so a change acts **from now on**: the past stays as it was and the future follows the new rates.

- Bleach a region, move away and come back: it is still dim. A place never lit is fresh (dSTORM dyes in their initial
  ON phase, PALM proteins unconverted, WideField dyes unbleached).
- PALM, 405 nm off: the proteins converted so far blink on until they bleach, no new ones follow; 405 nm up: the
  density of blinking dyes rises over the activation time instead of jumping.
- More (or less) laser power in dSTORM or WideField: the field continues from its present state at the new rates
  (WideField x4 power: the next frame is up to 4x as bright, a little less as the dyes bleach 4x faster within it;
  the dyes bleached so far stay bleached).
- DNA-PAINT: a change of imager concentration acts at once (the binding events are drawn per second; a second with
  a change takes each part at its own rate); the bindings before the change never change.
- Another dye or mode for a structure: the past light is re-read for the new dye (the new dye as it would be under the
  light the place has had).

What needs no light runs on the sample's time, lit or not: DNA-PAINT binding and PALM's spontaneous activation. What
the light drives (dSTORM switching, WideField and pre-state bleaching, PALM's 405 nm activation) advances with the dose:
a dye no light reaches now is frozen and emits nothing, and the beam's dim rim is lit and bleaches at its own, slower
rate.

Two of these changes, rendered by the cli with the same rate history (`--history-before`):

<!-- fig:photo-history -->

A frame counts its light (the whole beam, out to where it falls below 10<sup>-4</sup> of its peak) for one exposure
when the lasers' shutter is open, the light path carries power to the sample, and the frame is acquired (a snap or a sequence acquisition) or, with
`SampleHolder.TimeWhileIdle` = `Running` (the default), the shutter was opened by hand (Live off with the shutter open
still lights the sample, as on a real microscope; with autoshutter, the shutter is closed between acquisitions).
`Paused` stops the sample's time between acquisitions: no light, no drift, no binding. The sample's time starts
with its first light (before it nothing has happened to the sample). A snap takes a frame started after it was
called, a sequence acquisition frames started after it began, so the first frame after a stage move shows the new
place. The lit area follows the sample drift ([Camera](camera.md#drift)), and the background fade reads the
dose at the FOV centre. A stack reads the history once at its first frame and adds its frames, as one record of the
light along its own seeded drift path (so a stack is reproducible only on a freshly loaded device). The history is
cleared when the world changes (seed, cell parameters). Precision: the dose is bilinear between nodes 0.25 µm apart
(closer under a narrow beam, so it is within 0.1 % of the peak), its height dependence in 5 nm steps; a record keeps
the beam's place to 25 nm.
