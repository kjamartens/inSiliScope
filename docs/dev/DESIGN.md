# Viewer design system

The rules every change to the browser viewer (`web/index.html`, `web/live/`, `web/anim/`) follows. The look lives in
**design tokens**: change a colour, a size or the accent in one JSON file, never in the page.
Machine check: `node tests/web/design_lint.mjs` (CI job `viewer-js`, < 1 s).
Written 2026-11-05 after a review of the viewer against the guidance below (branch `viewer-design`).

## 1. Principles

Each rule cites its source.

1. **Two levels of disclosure at most.**
   - The settings level (Basic / Advanced / Expert, which follows Micro-Manager's tiers) is a filter.
   - A group is the only thing that folds.
   - Sub-rows (`kids`) indent one step, never two.
   - Never put an essential control only inside a folded group.

   (NN/g, Progressive disclosure; Accordions on desktop.)
2. **Workflow controls apart from settings.** Things touched while imaging (focus, exposure, light, display) are in the Acquire column. Things that describe the sample or the instrument are under the image. (Microsoft, Guidelines for app settings.)
3. **Every setting in exactly one place, with one name.** If a Micro-Manager config group sets what a viewer control already sets, the config group is left out (`mm_config_ui.js` `SKIP`) rather than shown twice. (Microsoft; Nielsen heuristic 4, consistency.)
4. **Group by spacing, not by boxes.** Leave more space between groups than inside them. No box-in-box borders. A thin line only separates sections or boxes of the column. (NN/g, Proximity.)
5. **Three type sizes** (caption, body, title), one spacing scale (4 px grid) and a few radii. (NN/g, Visual hierarchy; the 8-pt grid convention.)
6. **One accent colour, used only for meaning:**
   - the selected state;
   - a changed setting;
   - the focus;
   - a matching search row;
   - hover on a clickable name.

   It is never decoration. Status colours (live, ok, warn, error) are separate tokens and always come with a word or an icon, never colour alone. (Blender HIG, Colour; WCAG 1.4.1.)
7. **Dark, neutral chrome around images** is the default, because images are judged against their surround. A Light theme is offered. The image areas stay dark in every theme. (napari's default theme; NN/g, Dark mode.)
8. **Show what differs from the default, and undo it at every scale:**
   - the bar on a changed row;
   - ↺ on the row (or double-click its name);
   - ↺ on its group, with "n changed";
   - ☰ › Reset all;
   - the "Changed only" filter beside the search.

   (VS Code settings; Lightroom; napari.)
9. **Help on the name.**
   - A setting's help is its name's tooltip: a dotted underline marks it, and it opens on hover, on keyboard focus and by long press on touch.
   - It is 1–3 sentences, says what the setting does (not its default value), and names the Micro-Manager property in `<code>`.
   - Never put essential information only in a tooltip. (Blender HIG, Tooltips; NN/g, Tooltip guidelines.)
10. **Status is visible.** Anything over about 1 s shows that it is working, anything over about 10 s shows progress, and a long job can be cancelled (Record: click again; Save: Cancel). (NN/g, Progress indicators; Nielsen heuristic 1.)
11. **Accessible (WCAG 2.2 AA):**
    - text contrast ≥ 4.5:1;
    - controls, the focus ring and status marks ≥ 3:1;
    - a visible focus everywhere;
    - targets ≥ 24 × 24 px, or an equivalent control;
    - full keyboard access.

    The lint checks the token pairs' contrast in both themes and with every accent palette.

Sources:
- nngroup.com/articles/progressive-disclosure
- nngroup.com/articles/accordions-on-desktop
- nngroup.com/articles/gestalt-proximity
- nngroup.com/articles/visual-hierarchy-ux-definition
- nngroup.com/articles/the-power-of-defaults
- nngroup.com/articles/tooltip-guidelines
- nngroup.com/articles/progress-indicators
- nngroup.com/articles/ten-usability-heuristics
- nngroup.com/articles/dark-mode
- learn.microsoft.com/windows/apps/design/app-settings/guidelines-for-app-settings
- developer.blender.org/docs/features/interface/human_interface_guidelines
- code.visualstudio.com/docs/configure/settings
- w3.org/TR/WCAG22
- w3.org/community/reports/design-tokens/CG-FINAL-format-20251028

## 2. Tokens

The source is `web/design/tokens/*.tokens.json`, in the W3C Design Tokens Format Module 2025.10 structure: `$value`, `$type`, `$description`, and aliases `{group.token}`. Colours and dimensions are written as CSS strings, the format's draft form, which Style Dictionary also reads.

`node tools/gen_design_tokens.mjs` writes `web/design/tokens.css`. That file is committed; `--check` fails when it is stale. **Never edit `tokens.css`.**

| File | Tier | What |
|---|---|---|
| `primitives.tokens.json` | primitive | raw values: neutral greys, the accent palettes (`blue`, `teal`, `violet`, `amber`, `magenta`: 400 for dark, 600 for light), status hues, Okabe–Ito, font family and sizes, spacing, radii, shadows, z layers, durations |
| `accent.tokens.json` | primitive | **the accent**: `accent.dark` and `accent.light`, aliases into one palette |
| `semantic.dark.tokens.json` | semantic | what each colour is for: `color.bg.*`, `color.surface.*`, `color.border.*`, `color.input.*`, `color.text.*`, `color.accent.*`, `color.focus-ring`, `color.status.*`, `color.overlay.*` (over images), `color.panel.*` (the Microscope view's drawings) |
| `semantic.light.tokens.json` | semantic | the same names for Light; anything left out keeps its Dark value |
| `density.comfortable.tokens.json` / `density.compact.tokens.json` | semantic | control height, row and group gaps, padding, label column widths |

**CSS names:** the token's path joined by `-`, with a final `default` dropped. For example, `color.text.default` becomes `--color-text`, `color.text.muted` becomes `--color-text-muted`, and `font.size.caption` becomes `--font-size-caption`. An alias becomes `var(--…)`, so the themes and the accent act through the references.

**Layers in `tokens.css`:**
- `:root`: Dark and Comfortable.
- `:root[data-theme=light]`: Light.
- `:root[data-theme=system]`: Light inside `@media (prefers-color-scheme: light)`.
- `:root[data-density=compact]`: Compact.
- `.isc-dark`: the Dark set again, for the image areas (`#canvasHost`, `#liveView`, the phone's `#mStage`).

**At run time** (`web/design/theme.js`, loaded in `<head>`), `self.iscTheme` provides:
- `setTheme('dark'|'light'|'system')` and `setDensity('comfortable'|'compact')`, kept in `localStorage` (`isc.ui.theme`, `isc.ui.density`);
- `tok('--color-…')`, the value as resolved now, for canvas and SVG drawing;
- `isLight()`.

It also fires `isc-theme` on `window` whenever a value may have changed, and anything that draws colours must redraw on it (the histogram, the focus knob and the Microscope view's panels do). The settings file's *View* part saves `ui.theme` and `ui.density`; an old file's `ui.style` still loads.

Components use **semantic tokens only**. A primitive used in a component is a bug, apart from the density layout widths.

### How to …

- **Change the accent:**
  1. Point both aliases in `accent.tokens.json` at another palette, for example `{color.teal.400}` and `{color.teal.600}`.
  2. Run the generator and the lint (the lint checks contrast for every palette).

  To try one without editing a file, use `?accent=teal`, `?accent=%23e66a00`, or `localStorage.setItem('isc.ui.accent', 'violet')`. This is not offered in the UI (the user's choice, 2026-11-05).
- **Change a theme colour:** edit its semantic token, or add a primitive and point the semantic token at it. Then run the generator and the lint.
- **Add a theme** (for example high contrast):
  1. Add `semantic.<name>.tokens.json`.
  2. Add a block to `generate()` in the generator (`:root[data-theme=<name>]`).
  3. Add the name to `THEMES` in `theme.js` and a button to the Appearance control in the ☰ menu (`segOf('theme', …)` in `index.html`).
  4. Add the theme to the lint's loop.
- **Add a token:** give it a `$description` saying what it is for, and use it as `var(--…)` in CSS or `iscTheme.tok('--…')` in JS.

## 3. Regions

| Region | Element | Holds |
|---|---|---|
| Top bar | `#topBar` across the window: `#viewBar` (left) and `#liveBar` (right end) | the name, the left view's switch (`#leftSeg`: Sample \| Microscope), links; the settings level (`#levelSeg`), the settings-file icons, ☰ |
| Left view | `#canvasHost` / `#scopeHost` | the world view with its HUD (View ▾) and legend; or the Microscope view's panels (`cli/scope_panels.*` describes them, `web/live/scope_panels.js` draws them) |
| Live image | `#liveView` | the camera image; the zoom inset; the recording's player |
| **Microscope** | `#liveCtl` under the image, at its width: `#lv_cols`, a grid by the strip's width (one column; from 520 px the stage over the display beside the buttons over the acquisition; from 780 px stage \| buttons over display \| acquisition in one row, so all of them show under the largest image); the handle `#ctlUp` (a click, or the pointer resting on it 0.3 s) spreads it over the image's lower 75 % (`#sideMain.ctlUp`, folded back 0.1 s after the pointer leaves; a `--duration-fold` slide) | `#lv_acts`: a row across the strip: Live, Snap, Record, fresh sample, and at its right end the one-line status (`#lv_info`, full text in its tooltip, `#lv_busy`) (all in the bar on a phone); `#lv_stageBox` **Stage**: the joystick `#lv_joy` (drag: speed by the knob's distance, springs back; a click near an edge one step; arrow keys; Shift 10 steps / 10x the speed), the focus wheel `#mFocus` (▲ ▼, drag, scroll, click; the height under it as a number, its unit in the tooltip); a right-click on either opens `#lv_xyPop` / `#lv_zPop` (typed x, y, z and the steps) (a phone: the wheel as a ruler and the Z step, titled Focus, the Acquisition before the Display); `#lv_dispBox` **Display** beside the stage (`#lv_sd`): an upright level bar `#lv_lvl` (the grey scale `#lv_lut`, the histogram `#lv_hist`, a handle per level with its number right-aligned in a 5-digit column so "auto" in the title ends where the numbers end; a right-click opens `#lv_dispPop`); `#lv_acqBox` **Acquisition** (Micro-Manager presets: imaging mode, illumination, drift, quality; exposure); `#lv_colIllum` **Illumination** (light preset, laser powers, BFP position, pattern); `#lv_instrHost` the **instrument**: Light path, Objective & PSF, Camera (groups with `host: 'lv_instrHost'`) |
| **Settings** column | `#setCol` at the right edge: `#setColHead` (» folds it to the tab `#setTab`, `isc.ui.setShut`; the live image keeps its width, the left view takes the room), the search `#sideTop` (one search over every setting, and "Changed only"; typing unfolds the column), `#panel` | **Sample** (specimen first, then its structures), **Sample preparation** (labels, beads, custom dyes), **Simulation** (Performance (Expert), Image formation: how the simulator works, not the instrument), About |
| Menu | `#menu` | the settings file (save and load), Appearance (theme, density, the animation pane), Settings (expand or collapse all, reset all, the hidden-changes note), Tools, About |

A new setting goes in the region its job belongs to (rule 2), at the level its Micro-Manager property has (`tierOf`).
Never add a second control for something that already has one.

## 4. Components

All of them are styled in `web/index.html`'s `<style>` with tokens only.

| Component | Markup | Use | States, do and don't |
|---|---|---|---|
| Button | `<button>` | an action | hover: accent border; `:disabled`; focus ring. One style; do not pad buttons locally. Live and Record use `--color-status-live` (outline) and `-live-on` (fill while running). |
| Icon button | `<button aria-label title>` | ↺, ☰, ⤢, ✕ | always has an `aria-label` and a tooltip |
| Segmented control | `.seg > button[role=radio]` | 2–5 exclusive choices that are always visible (left view, level, theme, density) | `.on` or `aria-checked=true`; more choices → a select |
| Select | `<select>` | a longer list or a preset | a preset select shows "Custom" when edited by hand |
| Slider + number | `range` row (slider, bold value, unit) or `number` row (field, unit) | a continuous value | the unit is always visible; the wheel steps it |
| Two-knob slider | `.dual` (`pair` rows) | a min..max range | the boxes and the knobs stay together |
| Setting row | `.opt` (grid: `.lab`, `.ctl`, `.val`) | one setting | `.changed` (accent bar, ↺), `.hit` (search), `.t-advanced` and `.t-expert` (level); labels wrap to two lines, never an ellipsis |
| Group | `details.grp[data-group]` | the one folding level | its title row is the button: caret, title, `.grpChg` (n changed, ↺) |
| Section | `.sect` | the area's top-level headings (Sample, Simulation …) | a heading over a line; does not fold |
| Sub-heading | `.subhead` | a few rows inside a group | caption size, muted; use sparingly |
| Column box | `#liveCtl .lc > h5` | an Acquire box | looks like a group title; folds (`.shut`) |
| Readout | `.sub` under a control (`#bfpReadout`, `#patternReadout`) | what a value means now | text only, never an input |
| Pop-over | `.popover` | the menu, the Record box, the colour-map list | closes on Esc and on a click outside |
| Dialog | `iscSettings.dialog(html, onClose)` (`#setDlg`) | a choice or a long job (save, load) | a scrim (`--color-overlay-scrim`); Esc closes. Do not use `alert` or `confirm` in new code. |
| Tooltip | `#tip` (settings names, `data-tip`), `.pTip` (drawings) | help, a value | same look; opens on hover and focus; never essential |
| Overlay box | `#hud`, `#legend`, `#lv_play` | over the images | `--color-overlay-*`, dark in every theme |

## 5. Settings: words and levels

- **Sentence case** for every name: "Beam profile", not "Beam Profile" or "BeamProfile". Micro-Manager property names appear only inside `<code>` in the help.
- **Units:** put them beside the value (`.u`) and use SI symbols (µm, nm, kW/cm², ms), not in the name.
- **Names:** short enough to fit two lines of the label column (116 px under the image, 92 px in the column).
- **Levels:**
  - the tier is the Micro-Manager property's (`ISC_MM_TIERS`, `tierOf`);
  - settings Micro-Manager does not have take `adv` for Advanced;
  - Basic is what most people change in their first session.
- **Defaults** are a realistic experiment: most people never change them (NN/g, The power of defaults).
- Every setting has a default, so the changed mark and reset work (`resettable`).

## 6. Data and physics colours

These are not theme colours. Mark each line that has one with `// design:allow — <why>`.
- **A wavelength's colour:** `waveColour` in `scope_panels.js`.
- **The objectives' magnification code:** ISO 8038.
- **Colour maps:** the images' heat map and the world view's maps (`web/scene/`).
- **Categories:** use the Okabe–Ito tokens (`--color-okabe-ito-1..8`), 3–5 at a time, with the name beside each.
- **Continuous data:** use perceptually uniform maps (viridis, cividis, magma), never a rainbow.

**Known debt:** the world view's canvas strokes in `index.html`'s scripts (grid, focus plane, scale bars, the camera's frame) still use literal colours. They are on the always-dark canvas, and the lint does not scan `index.html`'s scripts yet. Move them to `color.overlay.*` / `tok()` when that code is next touched, then add the scripts to the lint.

## 7. Checklist for a new control or panel

- [ ] It sits in the region its job belongs to, at its Micro-Manager tier, and nothing else sets the same thing.
- [ ] Its name is in sentence case and its unit is shown; its help is 1–3 sentences with `<code>Device.Property</code>`.
- [ ] It has a default (the changed mark and the reset cover it).
- [ ] It uses the existing components and semantic tokens only; `node tests/web/design_lint.mjs` passes.
- [ ] It works from the keyboard, with a visible focus, in Dark, Light and Compact, and on a phone (`body.mobile`).
- [ ] Anything over 1 s shows that it is working, and a long job can be cancelled.
- [ ] Usage counts: a counted pick keeps its `data-pick` (docs/privacy.md lists any new event).
- [ ] The user-facing docs say the same: docs/try-viewer.md, the Quickstart's viewer tour.
