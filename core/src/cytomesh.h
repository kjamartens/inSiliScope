// Cytoplasm height field: analytic profile (cytoHeightAt) sampled on a polar
// mesh of shrunk outline copies, smoothed, and sampled bilinearly. Port of
// the JS prototype's cellOutlineLocal / nearestDistToOutline /
// nearestPointOnEllipse / nucleusSignedDistLocal / cytoHeightAt /
// buildCytoMesh / smoothCytoGrid / sampleCytoMeshHeight (index.html).
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

// JS Math.round (half-up) for the non-negative integers-as-doubles the
// prototype rounds (ring/theta counts, window sizes).
double JsRound(double x);

struct Pt2 { double x, y; };

std::vector<Pt2> CellOutlineLocal(const Cell& c, int n);
double NearestDistToOutline(const std::vector<Pt2>& pts, double x, double y);
Pt2 NearestPointOnEllipse(double a, double b, double x, double y);
double NucleusSignedDistLocal(const Cell& c, double lx0, double ly0);
double CytoHeightAt(const Cell& c, const Params& p, double dEdge, double dNuc);

struct CytoMesh {
   int rings = 0, n = 0;              // (rings+1) x n vertices
   std::vector<double> x, y, h;       // index k*n + i
   double H(int k, int i) const { return h[(size_t)k * n + i]; }
};

CytoMesh BuildCytoMesh(const Cell& c, const Params& p);   // outline n = max(8, round(cytoTheta))
double SampleCytoMeshHeight(const Cell& c, const CytoMesh& mesh, double x, double y);

} // namespace isc
