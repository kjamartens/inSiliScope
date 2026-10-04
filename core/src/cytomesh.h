// Cytoplasm height field: the analytic profile (cytoHeightAt) relaxed on a
// Cartesian grid (screened Poisson, nucleus obstacle; spec/ALGORITHM.md) and
// sampled bilinearly; a polar mesh of shrunk outline copies carries samples
// of it for drawing. Port of the JS prototype's cellOutlineLocal /
// nearestDistToOutline / nearestPointOnEllipse / nucleusSignedDistLocal /
// cytoHeightAt / buildCytoHeightGrid / buildCytoMesh / sampleCytoMeshHeight
// (index.html).
//
// Everything is in the cell's LOCAL frame (centre-relative, before packRot),
// a pure function of the cell's shape fields, so it is cached per cell.
// Uses Math.pow (ring spacing), so it is "near", not bit-exact, where the
// host libm's pow differs from the one Node was built against.
#pragma once

#include "cells.h"
#include "params.h"

#include <vector>

namespace isc {

constexpr double CYTO_RING_BIAS = 2.4;
constexpr double CYTO_GRID_UM = 0.25;
constexpr int CYTO_OUTLINE_N = 2048;
constexpr int CYTO_RELAX_SWEEPS[3] = { 40, 20, 20 };   // 4g, 2g, g

// JS Math.round (half-up) for the non-negative integers-as-doubles the
// prototype rounds (ring/theta counts, window sizes).
double JsRound(double x);

std::vector<Pt2> CellOutlineLocal(const Cell& c, int n);
double NearestDistToOutline(const std::vector<Pt2>& pts, double x, double y);
Pt2 NearestPointOnEllipse(double a, double b, double x, double y);
double NucleusSignedDistLocal(const Cell& c, double lx0, double ly0);
// The same with cos(-nucRot), sin(-nucRot) and, for a shaped nucleus, its
// footprint polygon (NucFootprintPolygon) passed in by a caller with many points.
double NucleusSignedDistLocal(const Cell& c, double lx0, double ly0, double cosNeg, double sinNeg,
                              const std::vector<Pt2>* poly);
double CytoHeightAt(const Cell& c, const Params& p, double dEdge, double dNuc);

// The relaxed height grid: nodes (i, j) at local ((i-half)*g, (j-half)*g),
// float as the JS Float32Array; outside nodes next to the outline hold
// ghost values (linear extrapolation through 0 at the boundary).
struct CytoHeightGrid {
   int N = 0, half = 0;
   double g = 0;
   std::vector<float> h;              // index j*N + i
};

struct CytoMesh {
   int rings = 0, n = 0;              // (rings+1) x n vertices
   std::vector<double> x, y, h;       // index k*n + i
   CytoHeightGrid hg;
   double H(int k, int i) const { return h[(size_t)k * n + i]; }
};

CytoHeightGrid BuildCytoHeightGrid(const Cell& c, const Params& p);
double SampleCytoHeightGrid(const CytoHeightGrid& hg, double x, double y);   // clamped at 0

CytoMesh BuildCytoMesh(const Cell& c, const Params& p);   // outline n = max(8, round(cytoTheta))
double SampleCytoMeshHeight(const Cell& c, const CytoMesh& mesh, double x, double y);

} // namespace isc
