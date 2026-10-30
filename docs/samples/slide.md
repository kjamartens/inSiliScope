# Fluorescent slide

A **fluorescent slide** is a uniform layer of dye on the coverslip, the sample used to see the beam's profile, to
flat-field a camera image or to check a filter set. It has no structure: the image is the excitation beam, the light of
every height of the layer collected out of focus, and how the layer bleaches where it is lit most.

Mount it with the viewer's **Specimen** select (**Fluorescent Slide**), Micro-Manager's `SampleHolder` (`Slide`) or the
CLI's `--specimen Slide` (3). Its four settings are the `FluorescentSlide` device's in Micro-Manager, the **Fluorescent
slide** group in the viewer and the `--slide-*` options of the CLI:

| Setting | Default | What it does |
|---|---|---|
| Thickness (`slide-thickness-um`, `ThicknessUm`) | 5 µm | The layer's thickness, from the coverslip up. |
| Dye (`slide-dye`, `Dye`) | Mix | **Mix**: TetraSpeck's four colours (blue 360/430, green 505/515, orange 560/580, dark red 660/680; the spectra of [Fiducial beads](fiducials.md#bead-types)), each as equivalent dyes (ε 10⁵, QY 0.5), so every channel sees it. Or one dye of the library, its fluorescent state. |
| Concentration (`slide-conc-micromolar`, `ConcentrationMicroMolar`) | 10 µM | Each dye's concentration in the layer (*estimate*). |
| Bleach half-time (`slide-bleach-half-sec`, `BleachHalfTimeSec`) | 900 s | The fluorescence half-time at 1 kW/cm² at the dye's own excitation peak (*estimate*); 0 = no bleaching. |

Its light preset is **561 nm at 0.02 kW/cm²** with the 600/37 filter set (`Slide-561`); with a library dye it is that
dye's channel at the same intensity (`Slide-405`, `Slide-488`, `Slide-561` or `Slide-640`: the filters of the dye's
widefield preset). The viewer sets it when the slide is mounted and when its dye is picked; in Micro-Manager it is the
`Lasers` preset `Auto` (a mount keeps the lasers as set; a dye pick applies `Auto`). At these defaults the centre of the
beam gives about 8800 photons per pixel per 50 ms frame (100 nm pixels; ~51 000 of the Kinetix22's 65 535 grey levels).
The viewer's world view draws the layer as a translucent slab: a tint from above, and in the x-z view a band from the
glass up to its thickness.

## The light

A dye at concentration c in a layer of thickness h gives each camera pixel of side a

  photons per second = c N<sub>A</sub> × a² × h × (the dye's detected photons per second at the local excitation),

with the dye's excitation, emission and detection through the light path as for any label
([Dyes and light path](../physics/dyes-and-light-path.md)). The layer's light is the free DNA-PAINT imager's model
([the DNA-PAINT imager background](../physics/dyes-and-light-path.md#the-dna-paint-imager-background)): every height of the layer, lit by the beam at that height, is
collected out of focus onto the pixel, over the objective's collection angles. Under a flat beam this is exactly the
column above the pixel, whatever the focus; under the default Gaussian beam the out-of-focus light softens the beam's
profile a little (more for a thick layer and a small field); in HILO the tilted sheet lights each height at another
place; in TIRF only the bottom 100-200 nm glows, so a thicker layer adds nothing. A thin layer and the beam's profile
are all the image shows: there is no PSF to see.

## Bleaching

The dyes bleach as their mean (deterministic, no single-molecule noise), at a hazard proportional to their excitation:
the half-time option at 1 kW/cm² at the dye's excitation peak, longer off the peak and at less light (at the default
0.02 kW/cm² at 561 nm the orange dye's half-time is about 12.5 h). Each pixel bleaches with the light its column has
had: in Micro-Manager and the viewer's live view the sample's [illumination history](../physics/photophysics.md#illumination-history-micro-manager-adapter), so a parked beam
burns its profile into the slide and moving the stage shows the bleached spot; a CLI movie starts at `start-sec` of its
own light. Each height of the column bleaches at its own excitation (HILO's tilted sheet, TIRF's depth), but the
bleached pattern is not blurred by the out-of-focus collection (the user's choice, 2026-10-30): exact for a layer
thinner than the bleached pattern's edges, a fraction of a µm of blur missing otherwise. The sample's drift does not
move the bleached pattern.

<!-- fig:slide-bleach -->

## Fiducial beads and BrightField

[Fiducial beads](fiducials.md) can be added on the glass as on any sample (their settings are the `Fiducials`
device's; a bead "on top of the sample" sits on the glass here). In BrightField the slide gives no contrast (the core
has no geometry for it): the frames hold the lamp and the camera noise only.

Code: `adapter/inSiliScope/Simulation/Slide.*` (the dyes, the bleaching), the map in `ScopeMovie.cpp`
(`FluorescenceMovie::Begin`); check: ctest `slide` (`cli/slide_check.cpp`).
