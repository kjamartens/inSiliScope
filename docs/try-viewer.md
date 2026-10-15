# Viewer (docs)

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
  in the same box. The microscope is on the right: its bar (level with the title bar) holds ▶ Live / Snap / ● Record /
  ↺ and one line with the frame rate, the time per frame, the image size and the pixel size (nm, one decimal); under it the live camera image, always square, with every
  sample and microscope setting below it at the same width (a search box filters the rows); beside them, down to the
  bottom of the window, a column of the microscope's controls that scrolls: the focus, the configuration, the
  illumination and light path, the objective and PSF (from the Advanced level: it has no Basic setting), the display
  range and, at the Expert level, the performance. The search box highlights every line that matches (at the current
  level), opens their groups and hides the rest of the settings. The divider between the
  two halves drags (the microscope takes half the window at first, a third to 62 % by dragging; remembered in your
  browser). On a narrow screen the parts stack.
- **Live** (in the bar, or the **▶ Live** in the empty image, which shows "Starting..." until the first frame) renders
  one camera frame after the other at the view centre, at most one per exposure time (the line beside
  the buttons shows the frame rate, the render time per frame, the image size and the pixel size, led by "Working..." while a
  slow frame, snap or recording is being rendered; the button turns red while live runs). It runs the adapter's live mode itself (`LiveScope`, in WebAssembly): the light a frame
  takes is added to the sample's **illumination history**, so dSTORM dyes leave their initial ON state, PALM dyes
  convert and bleach and widefield labels fade where you look, while a place you have not lit yet is fresh; moving away
  and back finds the dimmed place again. Brightfield frames take no fluorescence light. The drift walks on from frame
  to frame. A change of settings acts from the next frame on; a change that makes another world (seed, cell
  parameters, specimen) starts a fresh sample, and **↺** does so by hand. **Snap** takes one frame, **● Record** many (below;
  the Acquisition group is hidden for now). **Focus**: the arrows of the Z field (the only number field with arrows; the others take typing and the arrow keys), Page Up / Page Down or the
  mouse wheel over the image step it by the step size (beside it on the same line). **Display**: auto contrast (0.1-99.9 % of the pixels) or a fixed
  range, over a live histogram of the frame (log counts; its axes span the darkest to the brightest pixel and the
  tallest bar of the last 10 s of frames, so they move slowly; drag in it to move the nearer end of the display
  range). Live pauses while the page is hidden. The world view always outlines the camera's field (dashed white, "camera"), and the x-z
  view draws the **focal plane** across it, at the focus height in the sample's frame (a live drift moves both).
- **Performance** (its own box under Display, at the Expert level): how each label was drawn (SMLM splat: every
  blink placed with the PSF; binned SMLM: the same blinks on a finer grid, one FFT convolution, many blinks at once;
  mean-field widefield: no single blinks; or BrightField), the sample's time, how long this place has been lit and the
  drift; then where each frame's time goes, in milliseconds with one decimal: the engine's phases (setup, the core's
  blink events, splatting, camera noise, ...), the trip to and from the worker, the display, and the wait that paces
  frames to the exposure. Every row that can take time is always listed, in the same order; one the frame did not have
  shows "—".
- **● Record** opens a small box for the number of frames; its **Start** (or Enter) records N frames at the stage
  position with the current settings, rendered as fast as the engine goes, not paced to the exposure. The recording
  then plays in the live view in real time at the exposure it was taken with (100 frames of 1 ms play in 0.1 s; the
  screen shows the frame current at each refresh), with play/pause, a frame slider and ✕ to close it (Live, Snap or a
  new recording close it too),
  and **Save .tif** there saves it as one 16-bit TIFF stack, `insiliscope_live_<w>x<h>x<N>.tif`; nothing is saved by
  itself. A recording is a fresh sample: a movie starting at
  the Start time, the same one "Make movie here" makes and `insiliscope_cli --spec` renders from the spec in the
  file's header; the live sample is left as it was. The file opens in ImageJ/Fiji as a time series with the pixel size
  and frame interval set, and its header holds every setting (the engine spec). Clicking ● Record again cancels it.
  Live, Snap and ↺ wait while it records; a recording holds at most 1 GB of frames.
  With **continue the live sample** (in the box, Advanced level) the frames go on from the live sample instead: its
  illumination history, drift and camera noise run on through them, as in a Micro-Manager sequence acquisition. They
  are rendered in batches, each one movie on the sample's clock, so the slow parts of a frame (above all the
  blink-event query) are done once per batch (about 2 to 5 times faster per frame than live at 128-256 px); the last
  frame of each batch is shown, and clicking ● Record again stops early and keeps the frames so far.
- **Illumination & light path** (in the controls column, under Configuration): the light preset, each laser line as a
  slider with its power (kW/cm², three decimals; at the Basic level only the lines the labelled dyes use in their
  modes, with 405, 488 and 561 nm for PALM, and any line that is on), the background, geometry, filters and dichroic, and the spectra plot
  (dye excitation and emission, laser lines, filters, camera QE). Each laser line has the colour of its wavelength, and
  each dye's excitation (dashed) and emission (solid) the colour of its weighted emission wavelength; on the dark looks
  the lines are brighter and thicker. Click the plot for a larger copy with a legend of every line and what reaches
  the camera per label (detected fraction and wavelength, photons per second, activation, free-imager background;
  Esc or a click beside it closes it).
- **Configuration** (in the controls column): the config groups of the adapter's Micro-Manager configurations
  (`inSiliScope_Basic.cfg` at the Basic level, `_Advanced.cfg` at Advanced, `_Expert.cfg` at Expert): Mode (the
  configurations' Channel group, its presets named by the mode alone, without the dye they also pick: dSTORM,
  DNA-PAINT, PALM, WideField, BrightField, and in Advanced WideField + BrightField), Objective, Camera, Quality, Drift and Specimen (the configurations'
  laser-power groups are not shown here: the lasers are in Illumination & light path). A preset sets what the adapter sets for it (made by `tools/gen_mm_configs.py` with the configurations, from
  the adapter itself, so the two stay the same); the pixel size follows the objective and camera as in Micro-Manager
  (sensor pixel / (objective x 0.667 emission magnification): 97.45 nm at 100x with the Kinetix22). The select shows
  the preset the settings are at, or "—" once a setting differs. The pixel size is
  not a setting: it follows the camera and the objective (shown read-only under Camera, Advanced). The Camera group of
  the settings shows from the Advanced level on (at Basic the camera is the Configuration's, the image 128 px).
- The options are grouped the way a microscope is set up: sample (the specimen; for the cell field its cell
  shape, nucleus, cytoplasm, packing, microtubules; for the nanorulers their design, density and bending), sample
  preparation (labelling and fluorophore, refractive index), microscope
  (camera, image formation; illumination and the objective are in the controls column) and acquisition. Every group,
  here and in the controls column, is a box laid out like the Configuration (names in a column, the controls beside
  them, a number's unit right after it), open in full when the page loads; a click on its title folds it. Every option
  has a unit and a help text that shows when the pointer rests on its name. A setting that differs from its default
  carries a thin bar at its left.
- **Basic / Advanced / Expert** at the right end of the top bar (with the ☰ menu) choose which settings show, the same tiers as the Micro-Manager
  adapter's properties (each setting at the tier of the property that sets it, taken from the adapter when the
  configurations are made; a setting Micro-Manager does not have keeps the viewer's own level; Basic at first). A note says how many
  hidden settings differ from their defaults. The ☰ menu switches between three looks (Compact, Focus, the default,
  and Light); **Expand all** / **Collapse all** open or fold every box (the controls column's too), and **Reset all to defaults** puts every setting back, those beside the image too. The level and the
  look are remembered in your browser.
  Sliders also follow the mouse wheel, one step per notch.
- **Specimen** (Sample): **Cell field** or **Nanorulers** ([Samples](samples/index.md)). Switching shows that sample's
  groups and its label, and zooms to a scale that suits it (the nanorulers: 150 px/µm and closer, with their dyes on;
  zoomed further out, the dyes inside the camera's field are still all drawn, the others not).
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
  minimum spacing and the ring geometry. A mode or dye pick loads the dye's fields, the mode's labelling and the light
  preset of every labelled target combined (**Illumination & light path**: the preset, lasers and the spectra plot; dichroic, filter and,
  under Geometry, the DNA-PAINT chamber height in Advanced). The preset is also the modality: "BrightField (lasers
  off)" images transmitted light, every other preset fluorescence. The label stays
  available in BrightField. In Advanced, **Custom dyes** starts empty: **+ Add a dye** makes one (up to three) with its
  own name, a library dye to start from, a default mode and every dye value editable; it then appears in the Dye select.
  Dipole orientations other than Free are not implemented yet. Presets set several advanced options at once: **Cell shape** (footprint and outline), **Cell look** (cytoplasm height profile; the default is "Rounded") and **BF quality**. The options a preset drives are
  indented under it; editing one shows "Custom".
- Movies use the same PSF as the Micro-Manager adapter: scalar Gibson-Lanni + Zernike aberrations by default (the
  "PSF model" and "Aberrations" selects under Objective & PSF; Gaussian is the fast option). The kernel of the current
  PSF settings is computed in the background as soon as the page is ready and whenever those settings change, so a
  movie usually finds it ready. In Advanced, **Preview PSF** shows that kernel in the movie player: the oversampled
  kernel beside the camera-pixel image of one emitter at a pixel centre, with a slider for the emitter's distance from
  the focal plane.
  The viewer uses a 3 um kernel half width (the adapter: 7 um) to save browser memory; `--psf-kernel-half-width-nm 3000`
  reproduces a viewer movie with the CLI. Blinks also take the default halo cut (pixels below 3e-6 of the emitter's
  photons are left out) and skip a frame they give fewer than 50 photons ([Optics](physics/optics.md)).
- Widefield movies use WebGPU when your browser exposes a hardware adapter; the movie's info line says `GPU` or `CPU`.
  Everything falls back to the CPU on any GPU failure.
- **Drift** (Configuration) makes the sample drift: Off, Low, Medium, High or Extreme set the xy and z speeds (0-250
  nm/s) and a random-walk jitter (0-50 nm/√s), the directions drawn per noise seed. Advanced shows those four values,
  the directions, how far direction and z speed swing (and how slowly) (see
  [Camera and background](physics/camera.md#drift)). Drifting mean-field images (WideField labels) render on the CPU, a
  drifting brightfield movie on one worker.
- Brightfield movies run on the CPU, spread over the browser's workers (the condenser source points are shared out;
  the movie's info line says how many workers took part); the **BF quality** slider trades speed for precision (1: thin
  object, 6 condenser points ... 4: 0.25 um slices, 48 points). At 256 px the default level 3 takes well under a second
  once the cells are built (the first movie at a new place also builds the cells); repeating a movie is instant, and
  SMLM and widefield movies also reuse the cells of the previous movie.
- **Tilt** (0 = straight down, 90 = from the side) and **Rotation** (about the vertical through the view centre) turn
  the view; Shift-drag on the map does both (sideways turns, up and down tilts). **Z clip** draws only what lies
  between two heights above the coverslip; drag both knobs together for a slab moving through the cells (its full
  0-20 um range is off). The advanced **Turn height** sets the height of the point the view turns about.
- **Detailed cells** (default 5): only the cells nearest the view centre get their microtubules, nucleus and dyes;
  the others show their cytoplasm, so a zoomed-out, turned view stays fast. 0 draws every cell in full. Advanced:
  **Scope** draws all cells, the centre cell with faint neighbours, or the centre cell alone; **Look** switches between
  the depth colours and bright colours on black.
- The **x-z** checkbox (View, on by default) adds a side view along the bottom of the map: the cytoplasm, nuclei,
  microtubules and dyes of the view's depth range seen from the front (along y when the view is not turned), at equal
  horizontal and z scale; the Z clip shows as a band and an image slice's height as a dotted line. Its height follows
  the tallest cell in view (+ 1 µm), so a flat field leaves more room for the map.
- Tilted views are drawn in depth order: each cell's cytoplasm surface, nucleus and contour lines back to front, the
  cells far to near, and the microtubules and dyes depth-tested on the GPU, so a nucleus shows through the cytoplasm
  above it and a nearer cell covers a farther one.
- The **Animation** tab (work in progress: turn on "Show animation pane (WIP)" in the ☰ menu for the Settings /
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
  data (below): a **WideField slice**, **SMLM camera frames** or a **BrightField slice** riding on the sweeping plane,
  the **thresholded WideField** surface or the **SMLM localizations** in its wake (e.g. "Simulated up, WideField down,
  thresholded wake", or the ready-made "Simulated, WideField, thresholded" sequence); the export makes their data first
  (a sweep along the screen's depth shows an x-z slice). The data an animation needs is made by itself as soon as it is
  previewed or edited.
- **Simulated data** (hidden for now, with its Data acquisition settings; they are in View ▾, under the Show
  checkboxes, when the page sets `self.ISC_SHOW_DATA`; **Random cell** / **Previous cell** sit with Reset
  view, and in the Animation tab): simulated images of the cell nearest the view centre, drawn in 3D at the **slice
  height**. **Image slice** picks one: off, the WideField image with its focus there (the out-of-focus blur of the rest
  included), the WideField z-stack summed over z, the SMLM camera frames at the nearest focus position (blinking;
  **SMLM frames + localized emitters** adds a faint cross at the true position of every emitter localized in the frame
  shown), or the BrightField image. Picking a layer makes its data in the background straight away (for the centre
  cell, once per cell and settings); a status list under the layers shows each data set (WideField, SMLM frame and
  BrightField z-stacks, SMLM localizations) as ready, being made (with progress), queued or not made, and which layer
  needs it. The data are made one at a time; ✕ removes an item from the queue (it stays removed until "Make"), and an
  item whose cell leaves the view is removed by itself (it queues again when the cell is back at the centre). While an
  image slice is shown, its slice height and the movie's focus height (Acquisition) move together. The data use the movie settings
  (Microscope, Acquisition, the structure's label; the other targets are left out): WideField images the structure's dye in WideField mode, SMLM in the
  structure's mode when that blinks (dSTORM, PALM, DNA-PAINT; else the dye's own blinking mode), each with that dye
  mode's light preset when it differs from the panel's; labelling, camera and PSF are the panel's. SMLM planes start at
  the movie's start time (60 s by default). One movie per focus position over the cell's box, from 1 µm below the
  coverslip to 1 µm above the cell top (the slice height slider covers the same range); WideField and BrightField planes the
  mean of a few frames (advanced, Data acquisition: step, averaging, SMLM frames and step). They are kept in the browser, so a cell is
  acquired once per settings. "Crop to the cell" shows only the cell's own footprint. SMLM frames are drawn opaque;
  in the "Dark fluorescence" look the WideField slice adds as light (black is see-through) unless "Opaque slice" is
  on (in an animation step: the checkbox next to a slice's colour). "WideField, sum over z" shows the WideField z-stack summed over
  its planes instead of the plane at the slice height (in an animation step: the second checkbox of a WideField slice). SMLM planes start with fresh dyes
  by default (advanced: sequential, where bleaching dyes run out in later planes). **WideField thresholded** segments the
  cell from the WideField z-stack: out-of-focus light of a thin cell is nearly the same in every plane, so it thresholds
  the in-focus fine structure of each plane (Otsu's level x the Threshold slider) and fills each column from the
  coverslip up to its highest in-focus point. **SMLM localizations**
  emulate a multi-plane SMLM acquisition: at focus positions every SMLM step, 5000 frames each (advanced), every blink
  within the capture range (±400 nm) is localized per frame, displaced by its precision: "Precision xy" and "Precision z"
  (default 8 and 20 nm; an animation that shows localizations has its own in its Data section) are those of an in-focus blink ON for a whole frame (the dye's detected photons per second
  through the light path and camera), worse with fewer photons and with defocus; the blinks are the core's, with the
  label's kinetics; drawn coloured by height, or as Gaussian spots of their precision. Dyes are drawn in the dye's
  emission colour in every look, and an animation's legend names the dye and mode of each dye and data layer.
- The dashed white square on the map shows what the camera covers. A finished
  movie plays by itself at its exposure time per frame; the player starts in the bottom-right corner (over the x-z
  view) and its title bar drags it anywhere over the view.
- The movie player also saves the movie as a GIF or MP4 (as shown: the display range, scaled up to at least 512 px).
- Cells are packed on fixed blocks exactly as in the Micro-Manager adapter, so the view shows what the adapter images
  at the same position. The packed cell positions are remembered in your browser (a few hundred KB of local storage,
  one entry per seed and cell settings), so a reload shows the same field without packing it again.
- URL flags for testing: `?nw` (core on the main thread), `?2d` (force Canvas 2D), `?wfgpu=any` (allow software WebGPU).
- The original JS prototype the core was ported from is at [`prototype/`](../../prototype/index.html); it is
  the reference implementation that the golden vectors are frozen from.
