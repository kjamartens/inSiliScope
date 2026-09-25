#include "cells.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>

namespace isc {

namespace {
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
}

void NormalizeParams(Params& p)
{
   p.cellDiamMax = std::max(p.cellDiamMin, p.cellDiamMax);
   p.cellElongMax = std::max(p.cellElongMin, p.cellElongMax);
   p.cellHeightMax = std::max(p.cellHeightMin, p.cellHeightMax);
   p.nucLongMax = std::max(p.nucLongMin, p.nucLongMax);
   p.nucRatioMax = std::max(p.nucRatioMin, p.nucRatioMax);
   p.nucHeightMax = std::max(p.nucHeightMin, p.nucHeightMax);
   p.cytoRimHeightMax = std::max(p.cytoRimHeightMin, p.cytoRimHeightMax);
   p.cytoEdgeRiseMax = std::max(p.cytoEdgeRiseMin, p.cytoEdgeRiseMax);
   p.cytoMidHeightMax = std::max(p.cytoMidHeightMin, p.cytoMidHeightMax);
   p.cytoMidDistanceMax = std::max(p.cytoMidDistanceMin, p.cytoMidDistanceMax);
}

Cell RawCandidate(uint32_t seed, int32_t cx, int32_t cy, const Params& p)
{
   using namespace jsm;
   auto H = [&](uint32_t k) { return HashUnit(seed, cx, cy, k); };

   Cell c;
   c.cx = cx; c.cy = cy;
   const double S = p.chunkSize;
   const double margin = (1 - p.jitter) / 2;
   c.x = (cx + margin + p.jitter * H(CH::X)) * S;
   c.y = (cy + margin + p.jitter * H(CH::Y)) * S;
   c.present = H(CH::OCC) < p.density;
   if (!c.present) return c;

   // Geometric-mean radius from the size percentile; sizeT is reused for the
   // nucleus long axis (same percentile, not an independent draw).
   const double sizeT = H(CH::SIZE);
   const double baseR = lerp(p.cellDiamMin, p.cellDiamMax, sizeT) / 2;
   const double elong = lerp(p.cellElongMin, p.cellElongMax, H(CH::ELONG));
   c.semiMajor = baseR / sqrt(elong);
   c.semiMinor = c.semiMajor * elong;
   c.rot = H(CH::ROT) * PI * 2;
   c.height = lerp(p.cellHeightMin, p.cellHeightMax, H(CH::HEIGHT));
   for (int i = 0; i < N_HARM; i++) c.harmAmp[i] = H(CH::HARM_AMP + i) * p.cellBlob / HARM_K[i];
   for (int i = 0; i < N_HARM; i++) c.harmPh[i] = H(CH::HARM_PH + i) * PI * 2;

   // Area correction measured on the cell's own clamped outline (48 samples):
   // rescale so the mean r^2 matches the unmodulated ellipse. See spec
   // "Keeping diameter meaning the same size at any blobbiness".
   {
      const int N = 48;
      double sumBase2 = 0, sumR2 = 0;
      for (int i = 0; i < N; i++) {
         const double th = ((double)i / N) * PI * 2;
         const double phi = th - c.rot;
         const double base = (c.semiMajor * c.semiMinor) / hypot(c.semiMinor * cos(phi), c.semiMajor * sin(phi));
         double mod = 1;
         for (int j = 0; j < N_HARM; j++) mod += c.harmAmp[j] * cos(HARM_K[j] * th + c.harmPh[j]);
         mod = std::max(CELL_MOD_MIN, std::min(CELL_MOD_MAX, mod));
         sumBase2 += base * base;
         sumR2 += base * base * mod * mod;
      }
      const double k = sqrt(sumBase2 / sumR2);
      c.semiMajor *= k;
      c.semiMinor *= k;
   }

   const double nucLong = lerp(p.nucLongMin, p.nucLongMax, sizeT);
   const double nucRatio = lerp(p.nucRatioMin, p.nucRatioMax, H(CH::NUC_RATIO));
   const double offR = H(CH::NUC_OFFR) * p.nucOffsetFrac * baseR;
   const double offAng = H(CH::NUC_OFFANG) * PI * 2;
   c.nucLong = nucLong;
   c.nucShort = nucLong * nucRatio;
   c.nucHeight = nucLong * lerp(p.nucHeightMin, p.nucHeightMax, H(CH::NUC_HRATIO));
   c.nucOffX = cos(offAng) * offR;
   c.nucOffY = sin(offAng) * offR;
   c.nucRot = H(CH::NUC_ROT) * PI * 2;
   c.nucZ = c.height * lerp(0.4, 0.6, H(CH::NUC_ZFRAC)); // provisional, clamped by EnvelopNucleus

   // Per-cell cytoplasm targets, sampled before EnvelopNucleus (the slope
   // run-out needs cytoMidHeight).
   c.cytoRimHeight = lerp(p.cytoRimHeightMin, p.cytoRimHeightMax, H(CH::CYTO_RIM));
   c.cytoEdgeRise = lerp(p.cytoEdgeRiseMin, p.cytoEdgeRiseMax, H(CH::CYTO_EDGE));
   c.cytoMidHeight = lerp(p.cytoMidHeightMin, p.cytoMidHeightMax, H(CH::CYTO_MID));
   c.cytoMidDistFrac = lerp(p.cytoMidDistanceMin, p.cytoMidDistanceMax, H(CH::CYTO_MIDDIST));

   EnvelopNucleus(c, p.nucMargin, &p);

   c.height = std::max(std::max(c.height, c.cytoRimHeight), c.cytoMidHeight);

   // Packing reach: the clamped ceiling, not the unclamped harmonic sum.
   c.rOuter = c.semiMajor * CELL_MOD_MAX;
   c.priority = H(CH::PRIORITY);
   c.packRot = 0;
   return c;
}

double CellRadiusAt(const Cell& c, double thetaWorld)
{
   using namespace jsm;
   const double phi = thetaWorld - c.rot;
   const double a = c.semiMajor, b = c.semiMinor;
   const double base = (a * b) / hypot(b * cos(phi), a * sin(phi));
   double mod = 1;
   for (int i = 0; i < N_HARM; i++) mod += c.harmAmp[i] * cos(HARM_K[i] * thetaWorld + c.harmPh[i]);
   mod = std::max(c.modFloor, std::min(CELL_MOD_MAX, mod));
   return base * mod;
}

double CytoDomeReach(const Cell& c, const Params& p)
{
   const double margin = std::max(0.1, p.nucMargin);
   if (!(p.cytoDomeSlope > 0)) return margin;
   const double H = std::max(std::max(c.height, c.cytoRimHeight), c.cytoMidHeight);
   return std::max(margin, 1.5 * (H - c.cytoMidHeight) / p.cytoDomeSlope);
}

double CytoSlopeRunout(const Cell& c, const Params& p)
{
   const double margin = std::max(0.1, p.nucMargin);
   const double ceilRun = p.cytoMaxSlope > 0 ? 2 * c.cytoMidHeight * jsm::LN2 / p.cytoMaxSlope : 0;
   return std::max(0.0, CytoDomeReach(c, p) + ceilRun - margin);
}

// Keeps the nucleus ellipsoid inside the cell with >= margin clearance.
// Vertically grows height / re-clamps nucZ; laterally raises the per-cell
// modulation floor (not a whole-cell scale, see spec) and only scales the
// cell as a last resort.
void EnvelopNucleus(Cell& c, double marginUm, const Params* runoutParams)
{
   using namespace jsm;
   const double margin = std::max(0.1, marginUm);

   const double rz = c.nucHeight / 2;
   const double neededHeight = c.nucHeight + 2 * margin;
   if (c.height < neededHeight) c.height = neededHeight;
   c.nucZ = std::min(std::max(c.nucZ, rz + margin), c.height - rz - margin);

   // Evaluated after the vertical step: it needs the final c.height.
   const double extraUm = runoutParams ? CytoSlopeRunout(c, *runoutParams) : 0;
   const int N = 32;
   double neededFloor = CELL_MOD_MIN;
   for (int i = 0; i < N; i++) {
      const double th = ((double)i / N) * PI * 2;
      const double lx = cos(th) * (c.nucLong / 2), ly = sin(th) * (c.nucShort / 2);
      const double wx = c.nucOffX + lx * cos(c.nucRot) - ly * sin(c.nucRot);
      const double wy = c.nucOffY + lx * sin(c.nucRot) + ly * cos(c.nucRot);
      const double dist = hypot(wx, wy);
      const double angle = atan2(wy, wx);
      const double phi = angle - c.rot;
      const double base = (c.semiMajor * c.semiMinor) / hypot(c.semiMinor * cos(phi), c.semiMajor * sin(phi));
      if (base <= 1e-6) continue;
      const double required = (dist * 1.05 + margin + extraUm) / base;
      // Modulation at the sample's world angle around the cell, not `th`.
      double naturalMod = 1;
      for (int j = 0; j < N_HARM; j++) naturalMod += c.harmAmp[j] * cos(HARM_K[j] * angle + c.harmPh[j]);
      const double cappedNatural = std::min(CELL_MOD_MAX, naturalMod);
      if (cappedNatural < required) neededFloor = std::max(neededFloor, required);
   }
   c.modFloor = std::min(neededFloor, CELL_MOD_MAX);
   if (neededFloor > CELL_MOD_MAX) {
      const double k = neededFloor / CELL_MOD_MAX;
      c.semiMajor *= k;
      c.semiMinor *= k;
   }
}

} // namespace isc
