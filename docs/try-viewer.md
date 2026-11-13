# Viewer reference

Every part of the [viewer](../../){ target="_blank" } in detail. New to it? The [Quickstart](quickstart.md) walks
through it step by step.

<div class="isc-launch" markdown>
[Open the viewer :material-open-in-new:](../../){ .md-button .md-button--primary target="_blank" }
</div>


- **Layout.** The sample (the world view) is on the left; dragging it moves the stage, the wheel zooms (a drag in the
  live image moves the stage too: the sample follows the pointer). **Ctrl + wheel** over the live image zooms into it
  (up to 32x, about the pointer; a touchpad pinch too): an inset at its top left shows the factor and the part shown,
  a drag then pans within the image instead of moving the stage, and a click on the inset shows the whole image again. Its display
  options (layers, look, tilt and rotation, z clip) are under **View ▾** in the info box at its top left, which lists
  one item per line where the view is (seed, centre, scale, tilt) and what is still loading; View ▾ opens the options
  in the same box. One bar runs across the top of the window: ☰ and the save / load icons, then the Sample | Microscope
  switch at the left, the name in the middle, and Basic / Advanced / Expert at the right end. The microscope is on the right: the live camera image, always
  square, and below it at the same width the controls you use while imaging: a row with ▶ Live / Snap / ● Record / ↺ (it stays at the top while the controls under it scroll)
  and at its right end one short line with the frame rate and the time per frame (its tooltip adds the image size,
  the pixel size and a recording's message), then the **Stage** (a joystick: drag its knob and the field of view moves that way, faster the
  further you pull, and stops when you let go; a click near its edge moves one step, the arrow keys on it too; with
  Shift 10 steps, a drag 10 times as fast; beside it the focus wheel: ▲ ▼ at its ends, drag it or scroll over it (up =
  focus up), or click above or below its line; the focus height (µm) under it; a right-click on the joystick or the
  wheel opens a small box for the step and a typed position), the **Display** and the
  **Acquisition** (specimen, imaging mode, illumination, drift, quality, exposure). Under a large image they sit side by side,
  so all of them stay in view; below them the lasers and the
  BFP position, and the instrument (**Light path**, **Objective & PSF**, **Camera**). The small ▲ on the image's lower
  edge (a click, or the pointer resting on it) spreads these controls over the lower three quarters of the image; they fold back a tenth of a second after the
  pointer leaves them (or on ▼ or Esc). At the right edge, down to the bottom of the window, the **Settings** column (the specimen's settings):
  the sample, its preparation and, from the Advanced level, the **Simulation** (performance, image formation), with
  a search box on top. Its
  ⇥ folds it to a narrow **Show specimen settings** tab at the right edge, as it starts (the live image gets the room;
  the Sample or Microscope view keeps its size), and a click
  on the tab, or typing a search, opens it again. The search box highlights every line that matches (at the current
  level), opens their groups and hides the rest of the settings. The divider between the
  two halves drags (the microscope takes half the window at first, a third to 72 % by dragging; remembered in your
  browser).
- **Microscope view.** The **Sample | Microscope** switch in the title bar lays the microscope over the left half:
  on top the **Overview**, a drawing of the light path, lying down (the sample, the objective with its back focal plane just behind it, the dichroic with
  the lasers and their excitation filter coming in from below, the emission filter, the camera; each lit laser's light
  going to the sample and each label's emission coming back, in their wavelengths' colours, with arrows; a beam fades
  with what each part lets through and ends in a bar at a part that stops it, e.g. a far-red dye's emission at a
  600/37 emission filter), and the panels under it, two side by side: **Illumination profile** (a section seen from below at
  the focus, or at the height its slider sets, over 1.5x the field, and an x-z section through the axis with that
  height dashed; a frame slider under a structured-illumination pattern), **Point spread function (PSF)** (the kernel seen from above, zoomed
  to the square holding everything brighter than 1 % of the in-focus peak, about 1 um at NA 1.4, and an x-z section
  through it over the whole kernel (as the square root of the intensity, so the rings show), both scaled to the whole
  PSF's peak, so the view from above dims as the slider moves the emitter away from the focal plane),
  **Back focal plane (BFP)** (the pupil, the critical circle, each beam's spot with its polarization, the regime) and
  **Spectral info** (the filters, the dichroic, the camera's QE, each label's excitation (dashed) and emission, the laser
  lines; and what reaches the camera per label: detected fraction and wavelength, photons per second, activation,
  free-imager background; filled, with a wavelength axis, a legend in rows along the light path: lasers, excitation
  filter, dichroic, the sample's dyes, emission filter, camera, and under it what reaches the camera in the same rows; a pointer on a curve or a laser line marks it and its legend entry and tells its value at that wavelength, a pointer on a legend entry marks its curves, a row's name all of the row's). The tiles' square images are all one size; in its tile the drawing names the parts only. A
  pointer over a panel marks its parts in the drawing (the PSF: the objective and the camera; the spectra: the sample,
  the filters, the dichroic, the lasers and the camera). A pointer on a part or a beam of the drawing shows what it is
  at once (a laser's power at the sample and what the excitation filter and the dichroic pass; an emission's mean
  wavelength and what the dichroic, the emission filter and the camera pass) and marks it and the panels it belongs
  to. A custom laser line takes its place among the others by wavelength; a line the dichroic lets through is drawn
  going on past it. A click on a panel fills the left half with it at twice the
  size, with its captions and legend (✕ or Esc: back): the drawing then gives each part's product and setting, the
  lasers' powers and a legend of the beams (how much of each reaches the sample or the camera), and a click on a part
  opens its panel; the PSF becomes a 2 x 2 grid over the whole kernel, x-y and x-z, the kernel above the camera pixels (an emitter at a pixel centre), with a second slider
  that turns the x-z cut in the x-y plane (the cut drawn dashed on the x-y images from edge to edge, corner to corner
  at 45 degrees; an astigmatic PSF shows its two foci along x and y); its x-y images too are scaled to the whole PSF's
  peak, so they dim out of focus. The filled illumination profile and PSF carry scale bars. The mouse wheel over any image zooms it about the pointer (up to 32x; a drag pans, a small
  inset at its top left shows the part shown, a click on it the whole image); the PSF's two views from above zoom
  together, and the illumination profile is computed at the screen's own pixels for the part shown (smoothed where its
  stripes are finer than a pixel), so a structured-illumination pattern's fine stripes show cleanly, without moire,
  at any zoom. The panels follow every settings change. The switch is remembered in your browser; the sample
  keeps loading under the panels and is there as it was. A narrow window (a phone) stands the drawing up, the sample
  on top; the panels show in one column above the live image.
- **On a phone** (a window up to 700 px wide; a landscape phone or a tablet keeps the side-by-side layout) the live
  image comes first, square across the screen, with the world view as a small inset in its top-right corner: a tap on
  the inset swaps the two (remembered in your browser). ▶ Live / Snap / ● Record sit under the image and stay on screen
  while the controls and settings below scroll with the page. One finger on the live image moves the stage; two fingers
  moving up or down together move the focus (up = focus up), spread or pinched they zoom into the image. In the
  **Focus** box (above the Info, the Acquisition, then the Display, whose level bar lies down there) the focus wheel is a ruler: swipe it like a focus knob (left = up, one step per notch, a fast
  swipe goes further) or press − / + (held, they repeat). On the world view one finger pans; two fingers pinch to zoom,
  twist to turn the view, and moved up or down together they tilt it. A long press on a setting's name shows its help.
  The page starts at most two cell workers on a touch screen, to stay within a phone's memory.
- **Threads.** Live renders on several of your processor's cores: half its logical cores less one (5 on a 12-thread
  processor, at most 8), so one core stays free for the computer and one for the page; `?threads=N` sets the number. For that the page must be "cross-origin isolated":
  on your first visit it installs a small service worker (`coi_sw.js`, which adds two headers to the viewer page and
  touches nothing else) and reloads once. Where that is not possible (a private window in some browsers, or a browser
  without service workers) the viewer runs on one core and says so in a box at the bottom of the window ("Running on
  one thread..."; ✕ closes it for the session); a current Chrome, Edge or Firefox outside a private window runs
  threaded. Both give the same frames, bit for bit. `?threads=0` in the address runs on one core on purpose.
- **Live** starts by itself once the engine is up (the empty image shows "Starting..." until the first frame; it does
  not count as a press in the usage counts; `?live=0` in the address keeps it off) and renders
  one camera frame after the other at the view centre, at most one per exposure time (the line beside
  the buttons shows the frame rate, the render time per frame, the image size and the pixel size, led by "Working..." while a
  slow frame, snap or recording is being rendered; the button turns red while live runs). It runs the adapter's live mode itself (`LiveScope`, in WebAssembly): the light a frame
  takes is added to the sample's **illumination history**, so dSTORM dyes leave their initial ON state, PALM dyes
  convert and bleach and widefield labels fade where you look, at the beam's own shape and depth, while a place you have
  not lit yet is fresh; moving away and back finds the dimmed place again. Brightfield frames take no fluorescence light. The drift walks on from frame
  to frame. A change of settings acts from the next frame on; a change that makes another world (seed, cell
  parameters, specimen) starts a fresh sample, and **↺** does so by hand. **Snap** takes one frame, **● Record** many (below). **Focus**: the Stage's focus wheel (▲ ▼, a drag, a scroll, a click), the arrows of the Z field in its right-click box (the only number field with arrows; the others take typing and the arrow keys), Page Up / Page Down or the
  mouse wheel over the image step it by the step size (beside it on the same line). **Display** (beside the Stage): a level bar, upright: the grey scale
  the image is shown in, the frame's histogram (log counts, dark at the bottom; its axes span the darkest to the
  brightest pixel and the tallest bar of the last 10 s of frames, so they move slowly) and a ◀ handle per level with
  its number (ADU). Drag a handle, or anywhere on the bar to move the nearer level; a right-click types them. **auto**
  in its title (on by default) sets the range from each frame (0.1-99.9 % of the pixels); a drag turns it off. **Info**
  (left of the Stage): a button per laser line (lit in the line's colour when on; a click turns it off, or on again at
  its last power; at the Basic level the lines the labels use, and any line that is on), the setup in four rows of one
  line each (the labels' modes, the light's regime (Epi, HILO or TIRF; a HILO beam reaching over 100 µm deep counts as Epi; the depth in its
  tooltip), the NA and pixel size, the exposure; each row's tooltip
  has the details), and a warning line (its room kept when all is well, so nothing under the box moves): one warning's
  text, or several by short names side by side, the most serious first; a click on it lists each with what to do. It checks: live failed; all lasers off, or none reaching the sample; nothing labelled; a label
  no laser excites, or only weakly; a label whose light the filters block; saturated pixels; the focus below the
  coverslip, above the cells, off the nanorulers or the beads, or above the TIRF light; no cell in view; a laser line
  that excites no label; frames slower than the exposure. Live pauses while the page is hidden. The world view always outlines the camera's field (dashed white, "camera"), and the x-z
  view draws the **focal plane** across it, at the focus height in the sample's frame (a live drift moves both).
- **Performance** (Settings › Simulation, at the Expert level): how each label was drawn (SMLM splat: every
  blink placed with the PSF; binned SMLM: the same blinks on a finer grid, one FFT convolution, many blinks at once;
  mean-field widefield: no single blinks; or BrightField), the sample's time, how long this place has been lit and the
  drift; then where each frame's time goes, in milliseconds with one decimal: the engine's phases (setup, the core's
  blink events, splatting, camera noise, ...), the trip to and from the worker, the display, and the wait that paces
  frames to the exposure. Every row that can take time is always listed, in the same order; one the frame did not have
  shows "—". Under the times, the memory: the live worker's heap (it never shrinks, so this is the most it has held)
  and what the engine's caches hold, the largest first (the PSF kernel per label colour is usually most of it; a
  finer pixel size or a longer PSF z range makes it bigger). If a movie needs more memory than the browser gives it,
  the movie stops with an "out of memory" message and its worker starts afresh; the viewer goes on.
- **● Record** opens a small box for the number of frames and the sample's time at the first frame (**starting at**,
  default 0 s: the light just coming on, every dSTORM dye ON, nothing bleached or converted yet); its **Start** (or Enter) records N frames at the stage
  position with the current settings, rendered as fast as the engine goes, not paced to the exposure. The recording
  then plays in the live view in real time at the exposure it was taken with (100 frames of 1 ms play in 0.1 s; the
  screen shows the frame current at each refresh), with play/pause, a frame slider and ✕ to close it (Live, Snap or a
  new recording close it too),
  and **Save** there saves the movie as a 16-bit TIFF stack, or, with its **settings file** (reproduces the movie)
  and/or **ground truth** ([Ground truth](ground-truth.md)), one `.zip`; drop its `open_in_napari.py` on napari to view
  it ([napari](ground-truth.md#open-it-in-napari)). Nothing is saved by itself. A recording is a fresh sample: a movie starting at
  that time, the same one `insiliscope_cli --spec` renders from the spec in the file's header; the live sample is left as it was. The file opens in ImageJ/Fiji as a time series with the pixel size
  and frame interval set, and its header holds every setting (the engine spec). Clicking ● Record again cancels it.
  Live, Snap and ↺ wait while it records; a recording holds at most 1 GB of frames.
  With **continue the live sample** (in the box, Advanced level; the start time is then hidden) the frames go on from the
  live sample instead: its
  illumination history, drift and camera noise run on through them, as in a Micro-Manager sequence acquisition. They
  are rendered in batches, each one movie on the sample's clock, so the slow parts of a frame (above all the
  blink-event query) are done once per batch (about 2 to 5 times faster per frame than live at 128-256 px); the last
  frame of each batch is shown, and clicking ● Record again stops early and keeps the frames so far.
- **Illumination** (in the controls column, under Acquisition): the light preset, each laser line as a
  slider with its power (kW/cm², three decimals; at the Basic level only the lines the labelled dyes use in their
  modes, with 405, 488 and 561 nm for PALM, and any line that is on), the BFP position and the pattern. The beam's
  shape, the filters, the dichroic, the background and the BrightField lamp are the instrument's: **Light
  path** beside them (the spectra: the Microscope view's
  **Spectral info** panel, where each laser line has the colour of its wavelength and each dye's excitation (dashed) and
  emission (solid) the colour of its weighted emission wavelength, with what reaches the camera per label: detected
  fraction and wavelength, photons per second, activation, free-imager background;
  Esc or a click beside it closes it).
- **Acquisition** (in the controls column): the config groups of the adapter's Micro-Manager configurations
  (`inSiliScope_Basic.cfg` at the Basic level, `_Advanced.cfg` at Advanced, `_Expert.cfg` at Expert): Imaging mode (the
  configurations' Channel group, its presets named by the mode alone, without the dye they also pick: dSTORM,
  DNA-PAINT, PALM, WideField, SIM (the WideField labels under a SIM2D pattern at BFP position 0.75, 50 nm pixels, 100 ms
  frames; any other mode turns the pattern off and leaves the rest), BrightField, and in Advanced WideField +
  BrightField), Illumination, Drift and Quality,
  then the exposure; first of all the **Specimen** (the configurations' Specimen group, `SampleHolder`). Each setting has one place: the groups the settings already have a control for are not repeated
  here (Camera: the Camera group; Aberrations: Objective & PSF; the BFP position:
  Illumination), the Objective preset is the first row of Objective & PSF, and the laser-power groups are
  the lasers' sliders in Illumination. A preset sets what the adapter sets for it (made by `tools/gen_mm_configs.py` with the configurations, from
  the adapter itself, so the two stay the same). The select shows the preset the settings are at, or "—" once a
  setting differs.
- **Camera** (under the image, beside the acquisition controls): the camera, the field of view (px) and the
  **Magnification** between the objective and the camera (`EmissionPath.EmissionMagnification`; a camera preset sets its
  own: 0.667 with the Kinetix22, 1.6 with the iXon), with the total magnification under it. The pixel size follows as in
  Micro-Manager, sensor pixel / (objective x magnification), 97.45 nm at 100x with the Kinetix22; typing a pixel size
  sets the magnification that gives it. Advanced adds the camera's noise values.
- The options are grouped the way a microscope is set up: sample (the specimen; for the cell field its cell
  shape, nucleus, cytoplasm, packing, microtubules; for the nanorulers their design, density and bending), sample
  preparation (labelling and fluorophore, refractive index) and instrument (the light path, the objective and PSF, the
  camera, image formation). The controls column beside the image holds what you change while imaging: focus,
  acquisition, illumination, display. Every group is open in full when the page loads; a click on its title (anywhere
  on the row, ▸ before it) folds it. Every option has a unit and a help text that shows when the pointer rests on its
  name, or the keyboard focus is on it (a dotted underline marks a name with help). A setting that differs from its
  default carries a thin bar at its left and a ↺ that puts it back (a double-click on its name does the same); a
  group's title counts its changed settings and has a ↺ for them all. The search at the top looks through every
  setting in the Settings column; **Changed only** beside it shows just the settings that differ from their defaults.
- **Basic / Advanced / Expert** at the right end of the top bar choose which settings show, the same tiers as the Micro-Manager
  adapter's properties (each setting at the tier of the property that sets it, taken from the adapter when the
  configurations are made; a setting Micro-Manager does not have keeps the viewer's own level; Basic at first). A note says how many
  hidden settings differ from their defaults. The ☰ menu's **Appearance** sets the theme (Dark, the default; Light; or
  System, following your computer's setting; the world view and the live image stay dark in every theme) and the
  density (Comfortable or Compact); **Expand all** / **Collapse all** open or fold every box (the controls column's too), and **Reset all** puts every setting back, those beside the image too. Every setting, the level, the
  theme and the density are remembered in your browser across reloads, and so are the position in the sample and the zoom of the world
  view (Reset all forgets them, and puts the panel and x-z view sizes back).
- **Save and load settings.** The first section of the ☰ menu (and the two small icons beside it, a disk to save
  and a folder to load; not on a phone) keeps a setup as a file, `<name>.isc.json`, in parts that can be mixed: the
  **Instrument** (objective, PSF and aberrations, camera and its noise, pixel size, the beam's shape, custom laser
  lines and filter edges), the **Settings** (laser powers, the filters in place, the BFP position, the light paths,
  camera size, render quality), the **Sample** (specimen, seed, cells or nanorulers, every target's label and dye,
  custom dyes, beads, the medium), the **Measurement** (stage position, focus, exposure, drift, and the sample's light
  history: every live frame that lit it) and, unticked by default, the **View** (the world view's camera, layers and
  colours, the settings level, theme and density). Each row shows where the part came from ("edited" once its values moved); its tooltip
  adds a one-line summary and a short id of its values (the same values, the same id). **Save…** asks for a name and the parts;
  **Load…** (or a `.isc.json` dropped on the page) lists the file's parts and replaces only the ticked ones: another
  lab's instrument with your sample, or your instrument with someone's sample. Loading a sample leaves the lasers as
  they are; a loaded measurement replays its light history, so the next live frame is the one the saved session would
  have taken (bleached and blinking as it was). A file from another release loads with a note that the image may
  differ, and what the viewer could not take is listed. Every value is stored (defaults too) by name, and
  `insiliscope_cli --settings <file>` renders the same frames on the command line.
  The viewer counts anonymous usage (which specimen, mode and Look are picked, Live, Record, saved TIFFs), without
  cookies: [Privacy](privacy.md) lists what is counted and how to turn it off.
  A typed value takes effect 0.5 s after the last key, or at once on Enter. Sliders and number boxes also follow the
  mouse wheel, one step per notch.
- **Specimen** (Acquisition): **Cell field**, **Nanorulers**, **Bead sample** or **Fluorescent Slide** ([Samples](samples/index.md); the bead sample
  is a bare coverslip with only the fiducial beads: switching to it with no beads set puts 0.3 per µm² down in clumps
  (40 % aggregation), turns the 561 nm laser to 0.1 kW/cm² and the others off, draws the beads and opens the Fiducial
  beads group, where the diameter and the gel layer's thickness are Basic; the [fluorescent slide](samples/slide.md) is
  a uniform dye layer, its group **Fluorescent slide** holds its thickness, dye (default a mix of TetraSpeck's four
  colours), concentration and bleach half-time; switching to it, or picking its dye, sets the dye's channel at
  0.02 kW/cm², 561 nm for the mix; the world view shows its layer as a translucent slab, in the x-z view from the
  glass up to its thickness, and zooms to the camera's field). Switching shows that sample's
  groups and its label, and zooms to a scale that suits it (the nanorulers: 150 px/µm and closer, with their dyes on;
  zoomed further out, the dyes inside the camera's field are still all drawn, the others not). **Seed** (default 42,
  **Random** draws a new one) is the one seed of the simulation, as in Micro-Manager (`inSiliScope.RandomSeed`) and the
  cli (`seed`): the world comes from seed ^ 0x43454C4C, the blinking, drift and camera noise from the seed itself.
  The nanorulers are drawn from above as their bent rods (the **Nanorulers** checkbox, View) with the docking strands' dyes;
  the **Nanorulers** group sets the design, the density, the docking strands per spot (1-5 of the ring's 5) and, in
  Advanced, the bending (persistence length, default 1500 nm), the minimum spacing, the landing (attach height, roll)
  and the linker. A movie of the nanorulers wants the focus on the coverslip (Focus 0-0.02 µm, in the live panel; it stays at 0.5 µm when you switch).
- **Microtubule label** and **Nuclear pore label** (the cell field's two targets, imaged together; the nanorulers' is
  the **Docking strands label**, the same rows): the labelled share of the sites, then the mode, then the dye (the Dye
  select lists the dyes with data for that mode, and None: the target unlabelled), then the attachment (the probe:
  antibodies, nanobody, SNAP/Halo, ...; Advanced: its length, flexibility and linker) and the dipole orientation.
  Defaults: microtubules ATTO 655 DNA-PAINT through primary + secondary antibodies, pores Cy3B DNA-PAINT on SNAP-tagged
  Nup96. **Nuclear pores** sets their density on the nuclear envelope (default 8 per µm², at least 180 nm apart) and, in Advanced, the
  minimum spacing and the ring geometry. A third target, the **Nuclear lamina label**, is unlabelled by default (Dye
  None): pick a dye to show random lamina points on the nuclear envelope; **Nuclear lamina** (Advanced) sets their
  density (default 2000 per µm², 100 to 10 000) and the lamina-free disc around each pore (40 nm). A mode or dye pick loads the dye's fields, the mode's labelling and the light
  preset of every labelled target combined (**Illumination**: the preset and lasers; **Light path**: dichroic and
  filter; not the BFP position). The **BFP position** slider: 0 = epi, 1 = the critical angle, between HILO and above
  TIRF, with the regime and how deep the light reaches shown under it. Only the slider, the Illumination **Preset** (the
  DNA-PAINT presets HILO, on the nanorulers TIRF), Acquisition's **Illumination** (Epi / HILO / TIRF), the **Pattern**,
  a reset or a settings file move it; a dye, a mode, the Imaging mode or the specimen do not. With a pattern on, the BFP
  position below 0.05 (the beams at the centre: no fringes) sets the Pattern Off, and back at 0.05 or above the pattern
  returns (in Advanced the beam
  profile, its width and the DNA-PAINT chamber height; [Dyes and light path](physics/dyes-and-light-path.md)). The
  **Pattern** select makes the light several coherent beams in the back focal plane whose fringes step with the frame
  (structured illumination: SIM2D, SIM3D, LatticeSIM2D, LatticeSIM3D or Custom; a pick sets the BFP position to the
  type's radius and the polarization to S; the readout under it gives the fringe period and the frames per cycle; in
  Advanced the polarization, the plane the beams are in phase at (the focus, so the fringes move with the focus and
  the sample moves through them, or the coverslip), the phases and orientations per cycle, their order, the first
  orientation, a phase offset and the centre beam's amplitude; in Expert the phase drift and the **Beams** matrix: every beam of the
  illumination in every mode, one row per beam, `ux uy spot tiltX tiltY amp phase pol ellip phaseStep nm`, pre-filled
  with the type's beams, which typed rows replace; the column table and worked examples are on the physics page under
  [Structured illumination](physics/dyes-and-light-path.md#structured-illumination-sim)). The live sample counts its
  frames (the info line's tooltip shows the frame's place in the cycle, the performance box its angle and phase), a recording
  of a fresh sample starts at frame 0. The Microscope view's **Back focal plane (BFP)** and **Illumination profile** panels
  show the beams and the pattern they make for the current settings, each with a slider through the frame program. The preset is also the modality: "BrightField (lasers
  off)" images transmitted light, every other preset fluorescence. The label stays
  available in BrightField. In Advanced, **Custom dyes** starts empty: **+ Add a dye** makes one (up to three) with its
  own name, a library dye to start from, a default mode and every dye value editable; it then appears in the Dye select.
  Dipole orientations other than Free are not implemented yet.
- **Fiducial beads** (Sample, any specimen; off by default, density 0): beads per µm² of coverslip placed at random,
  then the bead **Type** (gold by default, TetraSpeck, FluoSpheres orange / red / dark red / infrared, red nanodiamonds;
  spectra and brightness after Balinovic et al. 2019), the **Height** (on top of the sample by default, on the
  coverslip, or in a gel layer of a given thickness; on the bead sample no "on top"), the **Aggregation** slider (the
  chance a bead is stuck to the next: clumps of touching beads, 0 % by default), in Advanced the **Diameter** (a type's
  pick sets its product size, 100 or 200 nm; any other, such as 20 nm, after it: the dyes scale with the volume) and its
  spread (12 %), the beads' PSF z range (14 µm), in Expert the
  bleach half-time (288 s at 1 kW/cm² at the dye's peak), the brightness as equivalent dyes, the filters' blocking OD
  for gold's scattered laser light and **Beyond the PSF stack** (Blur: a bead further off focus is a blur growing with
  the defocus; Not drawn: left out, faster) ([Fiducial beads](samples/fiducials.md)). The world view and its x-z side view draw
  them as discs of their diameter over the scene (the **Fiducial beads** checkbox, View); in the live image they are
  bright and do not blink, a defocused bead's light spreads over the whole frame (its PSF window is twice the field of
  view), and under the light they fade (a half-time of about 5 minutes at 1 kW/cm² at their peak), much more slowly off it.
- Presets set several advanced options at once: **Cell shape** (footprint and outline), **Cell look** (cytoplasm height profile; the default is "Rounded") and **BF quality**. The options a preset drives are
  indented under it; editing one shows "Custom".
- The live image uses the same PSF as the Micro-Manager adapter: a scalar Gibson-Lanni model with Zernike aberration
  modes by default (the "PSF model" and "Aberrations" selects under Objective & PSF; Gaussian is the fast option).
  From Advanced the box also has the objective's **NA** (the Objective preset, the first row of Objective & PSF, sets it; a value typed
  there makes the preset show "—", as Micro-Manager's turret goes to Custom), and in Expert the **Zernike modes**: the
  Aberrations preset's 28 coefficients in waves (OSA order 0-27), which you can type over (28 values, or 15 with the rest
  0); picking a preset replaces them again, as `Objective.ZernikeCoefficients` does in Micro-Manager. The
  kernel of the current PSF settings is computed in the background as soon as the page is ready and whenever those
  settings change. The Microscope view's **Point spread function (PSF)** panel shows that kernel: its core from above and an x-z section, with
  a slider for the emitter's distance from the focal plane; filled, the whole kernel and the camera-pixel image of one
  emitter at a pixel centre, each with an x-z section along a cut whose angle a second slider sets.
  The viewer uses a 3 um kernel half width (the adapter: 7 um) to save browser memory; `--psf-kernel-half-width-nm 3000`
  reproduces a viewer recording with the CLI. Blinks also take the default halo cut (pixels below 3e-6 of the emitter's
  photons are left out) and skip a frame they give fewer than 50 photons ([Optics](physics/optics.md)).
- **Drift** (Acquisition) makes the sample drift: Off, Low, Medium, High or Extreme set the xy and z speeds (0-250
  nm/s) and a random-walk jitter (0-50 nm/√s), the directions drawn per seed. Advanced shows those four values,
  the directions, how far direction and z speed swing (and how slowly) (see
  [Camera and background](physics/camera.md#drift)).
- Brightfield frames run on the CPU; the **BF quality** slider trades speed for precision (1: thin object, 6 condenser
  points ... 4: 0.25 um slices, 48 points).
- **Tilt** (0 = straight down, 90 = from the side) and **Rotation** (about the vertical through the view centre) turn
  the view; Shift-drag on the map does both (sideways turns, up and down tilts). The advanced **Perspective** (0-90°,
  the field of view, default 35°; 0 = orthographic) makes near things larger and far ones smaller, so a tilted floor
  recedes; the scale bar holds at the depth of the view centre. With perspective, **Distance haze** (75 %) fades cells
  and floor further away into a mist, and **Fade out far cells** (on) lets them fade out completely and draws nothing
  beyond about the eye's distance behind the view centre (fewer cells to load in a steeply tilted view). **Z clip** draws only what lies
  between two heights above the coverslip (a vertical slider, 0-6 µm; its full range is off); drag both knobs together
  for a slab moving through the cells. **Soft Z clip** (on) fades what lies just outside it over 0.2 µm, lines widening
  as if out of focus; **Follow live focus** keeps the clip on the live focal plane ± 0.3 µm. Surfaces are drawn as
  shells: a clip through the nucleus shows its rim. The advanced **Turn height** sets the height of the point the view turns about.
- **Detailed cells** (Advanced, default 5): only the cells nearest the view centre get their microtubules, nuclear
  pores and dyes; the others show their cytoplasm and nucleus, so a zoomed-out, turned view stays fast. 0 draws every cell in
  full. Advanced: **Scope** draws all cells, the centre cell with faint neighbours, or the centre cell alone; **Look**
  switches between the depth colours, bright colours on black (Dark fluorescence) and Dark fluorescence (depth, the
  default): on black, the microtubules and nuclear pores coloured by their height in the cell, the nucleus by its own
  height (drawn on the GPU, the 2D fallback keeps one colour for the microtubules). **Microtubule / Nuclear pore /
  Nucleus / Lamina colours** pick each one's colour scale (previewed in the list; defaults ice, warm, greys and greens),
  each shown only while its structure is drawn; the legend shows only what is drawn.
- **Nuclear pores** (View, on by default): each pore of a detailed cell as its two 8-fold rings on the nuclear envelope
  (cytoplasmic and nucleoplasmic, 50 nm apart, radius 53.5 nm), seen through their own tilt on the flanks of the
  nucleus; a pore is a dot when zoomed out, and from 400 px/µm each corner shows its four Nup96 sites.
- **Nuclear lamina** (View, off by default): the lamina of each detailed cell as a surface, labelled or not (also in
  the x-z view, filled): the nuclear envelope its points lie on, coloured by height (Lamina colours; the Look's = one
  colour). The points themselves are not drawn: at the default 2000 per µm² they are 22 nm apart, a few hundred
  thousand per nucleus. The surface's opacity follows their local density instead (the points within 0.3 µm of each
  vertex, relative to the set density, at most fully opaque): today's lamina is uniform, so the surface is evenly
  opaque; where a lamina is sparser it shows through. The density arrives a moment after the surface (the cell workers
  generate the points, about half a second per nucleus).
- The **x-z** checkbox (View, on by default) adds a side view along the bottom of the map: the cytoplasm, nuclei,
  microtubules and dyes of the view's depth range seen from the front (along y when the view is not turned), at equal
  horizontal and z scale; the Z clip shows as a band and an image slice's height as a dotted line. Its height follows
  the tallest cell in view (+ 0.5 µm), so a flat field leaves more room for the map; drag its top edge to set it (down
  to hide it), double-click it to fit again. Outside the Z clip it is drawn in grey.
- Tilted views are drawn in depth order: each cell's cytoplasm surface, nucleus and contour lines back to front, the
  cells far to near, and the microtubules and dyes depth-tested on the GPU, so a nucleus shows through the cytoplasm
  above it and a nearer cell covers a farther one.
- The **Animation** tab (beta: turn on "Show animation pane (beta)" in the ☰ menu for the Settings /
  Animation tabs) builds short animations of one cell for talks: the view orbits the cell
  while its structures appear. An animation is a list of **cycles**, each bound to a structure (microtubules, nucleus,
  cytoplasm) and made of **steps**. A step lasts some seconds, moves the camera (turning by the cycle's orbit, tilting,
  zooming) and can **sweep a plane** through the cell (up, down, side to side, or along the screen's axes) with a slab
  of some thickness; every layer of the step is shown **ahead** of the plane, **in the slab** and/or **behind** it (its
  wake), with its own colour and opacity. A step without a sweep makes its layers pop in (fading). Typical: the
  simulated microtubules building up bottom to top while the view turns, then a slab moving back down. Duplicate a cycle
  and switch its structure to repeat it for the nucleus; "keep shown after" leaves a layer on for later cycles. Ready-made
  cycles (27): orbits, fly-arounds, turntable, zoom dive, top-view reveal, peel away, x-ray, optical sections, depth scan,
  wipes, outside-in, dye sites, WideField / SMLM / BrightField slices, ground truth vs thresholded or localized, SMLM
  build-up; ready-made sequences under "New from…" (grand tour, ground truth vs data, an SMLM experiment, every
  modality, SMLM slab up / localizations down, turntable). The timeline under the view plays and
  scrubs it (Space, arrows, [ and ]; once, loop or ping-pong); the dimmed border shows the export frame. **Export**
  renders every frame off screen (not a screen recording) as MP4 (H.264; WebM where the browser cannot encode it), WebM
  or GIF, at 720p to 4K, with an optional scale bar, per-step captions, the plane's position and a legend. Animations
  are kept in the browser, with undo/redo, and saved or opened as `.json` files. Layers can also be simulated image
  data: a **WideField slice**, **SMLM camera frames** or a **BrightField slice** riding on the sweeping plane,
  the **thresholded WideField** surface or the **SMLM localizations** in its wake (e.g. "Simulated up, WideField down,
  thresholded wake", or the ready-made "Simulated, WideField, thresholded" sequence); the export makes their data first
  (a sweep along the screen's depth shows an x-z slice). The data an animation needs is made by itself as soon as it is
  previewed or edited.
- Cells are packed on fixed blocks exactly as in the Micro-Manager adapter, so the view shows what the adapter images
  at the same position. The packed cell positions are remembered in your browser (a few hundred KB of local storage,
  one entry per seed and cell settings), so a reload shows the same field without packing it again.
- URL flags for testing: `?nw` (core on the main thread), `?2d` (force Canvas 2D), `?wfgpu=any` (allow software WebGPU).
