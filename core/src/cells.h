// Per-chunk cell candidate: jittered position, blobby elliptical outline,
// nucleus ellipsoid, per-cell cytoplasm profile targets. Port of the JS
// prototype's rawCandidate / cellRadiusAt / envelopNucleus (index.html);
// spec/ (the prototype README) explains every non-obvious step.
#pragma once

#include "params.h"

#include <cstdint>

namespace isc {

// Hash purpose channels. Must match the JS `CH` table exactly.
// NUC_LONG (30) is retired on purpose: nucleus long axis reuses SIZE's draw.
namespace CH {
constexpr uint32_t X = 0, Y = 1, OCC = 2, SIZE = 3, ELONG = 4, ROT = 5, HEIGHT = 6,
   HARM_AMP = 10, HARM_PH = 20, NUC_RATIO = 31, NUC_HRATIO = 32,
   NUC_OFFR = 33, NUC_OFFANG = 34, NUC_ROT = 35, NUC_ZFRAC = 36, PRIORITY = 40,
   CYTO_RIM = 41, CYTO_EDGE = 42, CYTO_MID = 43, CYTO_MIDDIST = 44;
}

constexpr int N_HARM = 3;
constexpr int HARM_K[N_HARM] = { 2, 3, 5 };
constexpr double CELL_MOD_MIN = 0.35, CELL_MOD_MAX = 1.65;

constexpr int RADIUS_LUT_N = 128;
constexpr int OUTLINE_SAMPLE_N = 16;

struct Cell {
   int32_t cx = 0, cy = 0;
   double x = 0, y = 0;
   bool present = false;

   // Local shape (cell-centre frame, independent of x/y/packRot).
   double semiMajor = 0, semiMinor = 0, rot = 0, height = 0;
   double harmAmp[N_HARM] = {}, harmPh[N_HARM] = {};
   double modFloor = CELL_MOD_MIN;   // raised by EnvelopNucleus
   double nucLong = 0, nucShort = 0, nucHeight = 0;
   double nucOffX = 0, nucOffY = 0, nucRot = 0, nucZ = 0;
   double cytoRimHeight = 0, cytoEdgeRise = 0, cytoMidHeight = 0, cytoMidDistFrac = 0;
   double rOuter = 0;
   double priority = 0;

   // Packing state.
   double packRot = 0;

   // Packing caches (float, as the JS Float32Array versions).
   bool lutReady = false;
   float radiusLUT[RADIUS_LUT_N];
   float collisionLocal[OUTLINE_SAMPLE_N * 2];
};

Cell RawCandidate(uint32_t seed, int32_t cx, int32_t cy, const Params& p);
double CellRadiusAt(const Cell& c, double thetaWorld);
void EnvelopNucleus(Cell& c, double marginUm, const Params* runoutParams);
double CytoSlopeRunout(const Cell& c, const Params& p);
double CytoDomeReach(const Cell& c, const Params& p);

} // namespace isc
