// Per-chunk cell candidate: jittered position, blobby elliptical outline,
// shaped nucleus, per-cell cytoplasm profile targets. Port of the JS
// prototype's rawCandidate / cellRadiusAt / envelopNucleus (index.html);
// spec/ (the prototype README) explains every non-obvious step.
#pragma once

#include "params.h"

#include <cstdint>
#include <vector>

namespace isc {

// Hash purpose channels. Must match the JS `CH` table exactly.
// NUC_LONG (30) is retired on purpose: nucleus long axis reuses SIZE's draw.
// HEIGHT (6) is retired (the dome height follows the nucleus); NUC_ZFRAC (36)
// now draws the gap below the nucleus (nucBase).
namespace CH {
constexpr uint32_t X = 0, Y = 1, OCC = 2, SIZE = 3, ELONG = 4, ROT = 5,
   HARM_AMP = 10, HARM_PH = 20, NUC_RATIO = 31, NUC_HRATIO = 32,
   NUC_OFFR = 33, NUC_OFFANG = 34, NUC_ROT = 35, NUC_ZFRAC = 36, PRIORITY = 40,
   CYTO_RIM = 41, CYTO_EDGE = 42, CYTO_MID = 43, CYTO_MIDDIST = 44;
}

constexpr int N_HARM = 3;
constexpr int HARM_K[N_HARM] = { 2, 3, 5 };
constexpr double CELL_MOD_MIN = 0.35, CELL_MOD_MAX = 1.65;

// Fractal edge tail: harmonics TAIL_K0 .. TAIL_K0+N_TAIL-1 (6..64), amplitude
// u_k * cellRough * cellBlob/5 * (k/5)^-(2.5-D), D = cellFractalDim; applied
// as r *= 1 + softclamp(tail, TAIL_MAX). See spec/ALGORITHM.md "Edge roughness".
constexpr int TAIL_K0 = 6, N_TAIL = 59;
constexpr double TAIL_MAX = 0.3;
constexpr uint32_t TAIL_AMP_STREAM = 50, TAIL_PH_STREAM = 51;

// Nucleus shape (see "Nucleus shape" in cells.cpp and spec/ALGORITHM.md):
// lobes k = 2..NUC_K (radius), k = 1..NUC_K (thickness), soft-clamped at
// NUC_R_MAX / NUC_H_MAX; |nucAsym| <= NUC_ASYM_MAX.
constexpr int NUC_K = 8;
constexpr double NUC_R_MAX = 0.5, NUC_H_MAX = 0.6, NUC_ASYM_MAX = 0.9;
constexpr uint32_t NUC_SHAPE_STREAM = 52;   // hashStream base (TAIL_* use 50, 51)
constexpr int NUC_POLY_N = 256;             // footprint polygon points
constexpr int NUC_PUSH_ITERS = 24;          // bisection steps of NucPushOutLocal

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
   double tailAc[N_TAIL] = {}, tailAs[N_TAIL] = {};   // a_k cos(ph_k), a_k sin(ph_k)
   double tailBound = 0;             // bound on |tail|; 0 = no tail
   double nucLong = 0, nucShort = 0, nucHeight = 0;
   double nucOffX = 0, nucOffY = 0, nucRot = 0, nucZ = 0;
   // Nucleus shape. nucShaped false: the plain ellipsoid (every caller keeps
   // its ellipsoid code); the coefficient arrays are then unused.
   double nucBase = 0;                 // gap between the coverslip and the nucleus bottom
   bool nucShaped = false;
   double nucBend = 0, nucKDown = 1, nucKUp = 1, nucFBot = 0, nucFTop = 0;
   double nucUp = 0, nucDown = 0;      // vertical extents above/below nucZ
   double nucReach = 0;                // farthest footprint point from the nucleus centre (shaped)
   double nucCos = 1, nucSin = 0;      // cos/sin(nucRot) (shaped)
   double nucRc[NUC_K + 1] = {}, nucRs[NUC_K + 1] = {}, nucHc[NUC_K + 1] = {}, nucHs[NUC_K + 1] = {};
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

struct Pt2 { double x, y; };
struct Pt3 { double x, y, z; };

Cell RawCandidate(uint32_t seed, int32_t cx, int32_t cy, const Params& p);
double CellRadiusAt(const Cell& c, double thetaWorld);
double CellTailAt(const Cell& c, double theta);
// A lower bound on CellRadiusAt at any angle (ellipse minor axis x floor x
// (1 - tailBound), minus a relative 1e-12 for rounding): a point closer to
// the centre is inside without evaluating the outline. Speed only.
inline double CellInnerRadiusBound(const Cell& c)
{
   const double b = c.semiMinor < c.semiMajor ? c.semiMinor : c.semiMajor;
   return b * c.modFloor * (1 - c.tailBound) * (1 - 1e-12);
}
void EnvelopNucleus(Cell& c, double marginUm, const Params* runoutParams);

// Nucleus shape: footprint radius s (1 = the outline), direction (C, S) =
// (cos t, sin t), height zeta in [-1, 1]. Port of the prototype's
// nucFootR / nucThickAt / nucSectionW / nucColumnExt / nucMapLocal /
// nucBallLocal / nucleusColumnLocal / nucPushOutLocal / nucleusRingsLocal.
double NucFootR(const Cell& c, double C, double S);
double NucThickAt(const Cell& c, double s, double C, double S);
double NucSectionW(double zeta, double f);
double NucColumnExt(double s, double f);
Pt3 NucMapLocal(const Cell& c, double s, double C, double S, double zeta);   // cell-local
struct NucBall { double s, C, S, H; };
NucBall NucBallLocal(const Cell& c, double lx, double ly);
// The nucleus chord through cell-local (lx, ly): extents below/above nucZ;
// false outside the footprint.
bool NucleusColumnLocal(const Cell& c, double lx, double ly, double& below, double& above);
// Shaped nucleus only: pushes `pt` out of the nucleus if inside (radially in
// (s, zeta), the boundary scale divided by `margin`). True if it moved.
bool NucPushOutLocal(const Cell& c, Pt3& pt, double margin);
// The footprint polygon (NUC_POLY_N points, cell-local; shaped only).
std::vector<Pt2> NucFootprintPolygon(const Cell& c);
// Drawing rings: `slices` horizontal sections of `pts` points, bottom to top,
// zeta = -cos(pi i / (slices - 1)), both poles included; xyz, cell-local.
std::vector<Pt3> NucleusRingsLocal(const Cell& c, int slices, int pts);
double CytoSlopeRunout(const Cell& c, const Params& p);
double CytoDomeReach(const Cell& c, const Params& p);

} // namespace isc
