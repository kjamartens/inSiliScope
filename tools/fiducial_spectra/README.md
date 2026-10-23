# Fiducial-marker spectra and brightness

Tools behind `data/fiducials/` (the fiducial beads: types, spectra, gold Mie table). BSD-3-Clause.

| Tool | What |
|---|---|
| `curves.mjs` | the paper curves on the 300-900 nm grid (`curveValues`), the quoted efficiencies from them |
| `fit.mjs` | fits each curve's tail and each calibrated component's `emitters`; writes both back |
| `gold_mie.mjs [--check]` | gold sphere scattering / absorption cross sections (Mie, BHMIE) -> `gold_mie.json` |
| `check.mjs` | quoted efficiencies within tolerance, fits and Mie table current (CI job `lab-check`, < 1 s) |

## How the curves were read

Balinovic, Albrecht & Endesfelder 2019 (`balinovic2019`, J. Phys. D 52, 204002, CC BY 3.0) publish the markers'
spectra only as figures 2a-6a. The high-resolution JPGs (`content.cld.iop.org/.../dab0862f0N_hr.jpg`) were read by
eye: each panel's axes calibrated from its tick marks (pixel columns and rows of the dark ticks found by script), a
grid overlay every 5 nm and 10 % drawn on a 3-6x enlargement, the curve's points noted at its turning points and
every few nm. Accuracy about +-1 nm and +-2 % of the peak. The figures themselves are not committed.

The paper's ET610/75 band is drawn at 568.5-642.5 nm (the grey box in figs 2a, 3a, 4a, 6a), 4 nm bluer than the
filter's nominal 572.5-647.5; the quoted collection of gold (70 %) and the TetraSpeck orange dye (85 %) needs the drawn
edge (60 % and 76 % with the nominal one), so the checks use the band as drawn.

## Tails

Efficiencies of 0.1-1 % (a red-shifted marker excited or detected far off its peak) are below what a linear plot
shows. A curve with a `tail` gets an exponential below its first point; `fit.mjs` fits its decay length to the quoted
value. Dark red: one decay for three filters (2 / 0.7 / 0.1 % quoted, 1.6 / 1.2 / 0.09 % fitted).

## Brightness

A component shines like `emitters` equivalent dyes (eps 100000 /M/cm, QY 0.5) at the type's reference diameter.
`fit.mjs` takes it from the paper's max-pixel histograms: the bead's maximum pixel above the background over a single
reference molecule's (mEos3.2 at 561 nm, JF646 at 640 nm) in the same images, through the same laser and filter, times
the reference's eps x QY x read-out over the equivalent dye's eps x QY x the bead's excitation and emission efficiency
from its curves. The simulator then reproduces the paper's bead-to-molecule ratios under the paper's conditions. Where
an efficiency is a tiny tail value, the count is an equivalent, not a dye count (the infrared beads come out at ~4e6
equivalent dyes in 100 nm: the paper's 0.1 % / 0.2 % tails underestimate the real leak).
