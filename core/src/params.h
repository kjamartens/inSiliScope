// Cell-field parameters. Defaults = the JS prototype's UI defaults
// (index.html inputs, after params()'s own min/max normalisation).
#pragma once

#include <cstdint>

namespace isc {

struct Params {
   // Field
   double chunkSize = 26;
   double jitter = 0.8;
   double density = 0.33;   // chunk occupancy
   // Cell
   double cellDiamMin = 25, cellDiamMax = 35;
   double cellElongMin = 0.5, cellElongMax = 1;
   double cellBlob = 1.75;
   double cellRough = 0.15;        // fractal edge tail amplitude (x cellBlob); 0 = off
   double cellFractalDim = 1.35;   // box-counting dimension of the tail, [1, 2]
   // Nucleus
   double nucLongMin = 8, nucLongMax = 12;
   double nucRatioMin = 0.6, nucRatioMax = 1;
   double nucHeightMin = 0.2, nucHeightMax = 0.3;
   double nucOffsetFrac = 0.1;
   double nucMargin = 0.5;          // sides and top
   double nucBaseMin = 0.4, nucBaseMax = 0.9;       // gap between the coverslip and the nucleus bottom (um)
   // Nucleus shape (cells.h "Nucleus shape"; all 0, nucAsym 0 and widest 0.5 = the plain ellipsoid)
   double nucIrregMin = 0.03, nucIrregMax = 0.2;    // rms relative radius deviation
   double nucBendMin = 0, nucBendMax = 0.3;         // kidney bend
   double nucSmooth = 2.5;                          // spectral slope of the lobes
   double nucThickIrreg = 0.1;                      // rms relative thickness variation at the edge
   double nucAsym = 0.5;                            // > 0 a wider base, < 0 a wider top (-0.9..0.9)
   double nucWidestMin = 0.2, nucWidestMax = 0.4;   // height of the widest section (fraction of the height)
   // Cytoplasm: the viewer's "Rounded" look since 2026-10-03 (was rim 0.1-0.3, mid 1-2, slope caps 1 and 3)
   double cytoRimHeightMin = 0.2, cytoRimHeightMax = 0.5;
   double cytoEdgeRiseMin = 0.1, cytoEdgeRiseMax = 0.5;
   double cytoMidHeightMin = 2, cytoMidHeightMax = 3.5;
   double cytoMidDistanceMin = 0.1, cytoMidDistanceMax = 0.3;
   double cytoMaxSlope = 2;
   double cytoDomeSlope = 4;
   double cytoRelaxUm = 1;         // screened-Poisson relaxation length; 0 = raw profile
   double cytoRings = 60;
   double cytoTheta = 256;
   // Packing
   bool enablePacking = true;
   bool allowPackRotation = true;
   double packFrac = 1.0;
   double relaxIters = 80;
   double relaxDamping = 0.55;
   // Microtubules (microtubules.js)
   double mtDensity = 0.9;          // per um^2 of footprint
   double mtStartDecayPct = 1.6;    // start density ~ exp(-gap to the nucleus / L), L = this % of the equivalent diameter
   double mtEndDecayPct = 20;       // end density ~ exp(-gap to the outline / L), same L convention
   double mtDirKappa = 1.5;         // end direction weight exp(kappa (cos a - 1)); 0 = any
   double mtWobbleTurn = 0.8;
   double mtWobbleFactor = 1.05;
   double mtStepLen = 0.05;
   double mtSmoothLen = 1.5;
   double mtMinTurnRadius = 0.15;
   double mtMinSeparation = 0.05;
   double mtMaxZSlope = 5;
   // Nuclear pores (issue 15, npcs.h; ABI 13): per um^2 of envelope (the user's choice 2026-10-12, above the measured
   // 5-8.6 of maeshima2006 and dultz2010), minimum centre distance (the NPC diameter, maeshima2006), the Nup96 ring
   // radius (webSMLM; thevathasan2019: 53.7), the two rings' separation along the pore axis and the corners' arc spread.
   double npcDensity = 8;            // per um^2 of envelope (user 2026-10-13; was 12; measured 5-8.6)
   double npcMinDistNm = 180;        // nm between pore centres (user 2026-10-13; was 120 = a pore diameter)
   double npcRadiusNm = 53.5;
   double npcRingSepNm = 50;
   double npcCornerSpreadNm = 12;
   // The labels (density, fluorescent fraction, mode, kinetics) are not
   // params since issue 16 (ABI 10): World::SetLabel, isc_world_set_label.

   // Specimen (ABI 12; data/specimens.json): 0 the cell field above, 1 the
   // DNA-origami nanorulers (origami.h; JS origami.js ORIGAMI_DEFAULTS).
   double specimen = 0;
   double origamiDesign = 0;            // index into data/origami/designs.json
   double origamiSpacingNm = 80;        // lattice step between neighbouring spots, nm
   double origamiDensity = 1.2;         // molecules per um^2 before thinning (estimate)
   double origamiMinSpacingNm = 0;      // Matern-II hard core between molecule centres (0 = none; 250 until 2026-10-12)
   double origamiBend = 1;              // 1 = the design's deformation, 0 = rigid
   double origamiLpNm = 1500;           // persistence length, nm (estimate; chhabra2020)
   double origamiStrandsPerSpot = 5;    // ring positions 0..N-1 of each spot carry a strand
   double origamiAttachNm = 7;          // the body's bottom above the glass, nm (estimate)
   double origamiLinkerNm = 3;          // docking-strand linker half-ball radius, nm (estimate)
   double origamiRoll = 0;              // 0 uniform roll about the axis, 1 fixed at origamiRollDeg
   double origamiRollDeg = 0;
   double origamiHeights = 0;           // 0 = 2D (every site at the axis height), 1 = the ring's heights

   // Fiducial beads on either specimen (ABI 15; fiducials.h; JS fiducials.js FIDUCIAL_DEFAULTS). Sites only: their
   // light is the engine's.
   double fidDensity = 0;               // beads per um^2 of coverslip (0 = none)
   double fidDiameterNm = 200;          // mean diameter, nm
   double fidDiameterCvPct = 12;        // diameter spread, % (estimate)
   double fidHeight = 1;                // 0 Coverslip, 1 OnTop (default), 2 Layer
   double fidLayerUm = 10;              // Layer: the gel's thickness, um
};

// params() in the prototype clamps every "max" to at least its "min",
// and floors a few microtubule controls (mtWobbleFactor >= 1, ...).
void NormalizeParams(Params& p);

// Set a field by its (JS prototype) name; booleans take 0/1. False if unknown.
bool SetParam(Params& p, const char* name, double value);

// A 64-bit hash of the fields a cell's packed pose depends on: everything
// but the microtubule (mt*) and nuclear pore (npc*) parameters (and the labels, not params), which only
// shape what hangs off a packed cell (the viewer's pack key makes the same
// cut), the specimen's (specimen, origami*), which the cell field never
// reads, and the fiducial beads' (fid*), which move no cell. Keys the packed-block store (blockstore.h).
uint64_t PackingFingerprint(const Params& p);

} // namespace isc
