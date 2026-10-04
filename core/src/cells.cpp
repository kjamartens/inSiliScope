#include "cells.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <limits>

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
   p.mtStartFracMax = std::max(p.mtStartFracMin, p.mtStartFracMax);
   p.mtEndFracMax = std::max(p.mtEndFracMin, p.mtEndFracMax);
   p.mtWobbleFactor = std::max(1.0, p.mtWobbleFactor);
   p.mtStepLen = std::max(0.02, p.mtStepLen);
   p.mtSmoothLen = std::max(0.0, p.mtSmoothLen);
   p.mtMinTurnRadius = std::max(0.0, p.mtMinTurnRadius);
   p.mtMaxZSlope = std::max(1.0, p.mtMaxZSlope);
}

namespace {
// The tail's spectral factors (k/5)^-tailExp, k = TAIL_K0 .. TAIL_K0+N_TAIL-1,
// as exp(-tailExp * log(k/5)) (fdlibm, bit-exact; not pow): a pure function
// of the parameters, tabulated once per thread per tailExp instead of per
// candidate.
const double* TailFactors(double tailExp)
{
   thread_local double cachedExp = std::numeric_limits<double>::quiet_NaN();
   thread_local double table[N_TAIL];
   if (!(cachedExp == tailExp)) {
      for (int i = 0; i < N_TAIL; i++)
         table[i] = jsm::exp(-tailExp * jsm::log((double)(TAIL_K0 + i) / 5));
      cachedExp = tailExp;
   }
   return table;
}
} // namespace

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
   // Fractal tail; (k/5)^-e as exp(-e*log(k/5)) (fdlibm, bit-exact; not pow).
   if (p.cellRough > 0 && p.cellBlob > 0) {
      const double tailExp = 2.5 - std::min(2.0, std::max(1.0, p.cellFractalDim));
      const double* tail = TailFactors(tailExp);
      HashStream nextA(seed, cx, cy, TAIL_AMP_STREAM), nextP(seed, cx, cy, TAIL_PH_STREAM);
      double sum = 0;
      for (int i = 0; i < N_TAIL; i++) {
         const double u = nextA.Next();
         const double ph = nextP.Next() * PI * 2;
         const double a = u * p.cellRough * p.cellBlob / 5 * tail[i];
         c.tailAc[i] = a * cos(ph);
         c.tailAs[i] = a * sin(ph);
         sum += a;
      }
      c.tailBound = std::min(TAIL_MAX, sum);
   }

   // Area correction measured on the cell's own clamped outline (48 samples):
   // rescale so the mean r^2 matches the unmodulated ellipse. See spec
   // "Keeping diameter meaning the same size at any blobbiness".
   {
      const int N = c.tailBound > 0 ? 256 : 48;
      double sumBase2 = 0, sumR2 = 0;
      for (int i = 0; i < N; i++) {
         const double th = ((double)i / N) * PI * 2;
         const double phi = th - c.rot;
         const double base = (c.semiMajor * c.semiMinor) / hypot(c.semiMinor * cos(phi), c.semiMajor * sin(phi));
         double mod = 1;
         for (int j = 0; j < N_HARM; j++) mod += c.harmAmp[j] * cos(HARM_K[j] * th + c.harmPh[j]);
         mod = std::max(CELL_MOD_MIN, std::min(CELL_MOD_MAX, mod));
         if (c.tailBound > 0) mod *= 1 + CellTailAt(c, th);
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
   c.rOuter = c.semiMajor * CELL_MOD_MAX * (1 + c.tailBound);
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
   if (c.tailBound > 0) return base * mod * (1 + CellTailAt(c, thetaWorld));
   return base * mod;
}

// z^k = e^{ik theta} by complex multiplication (only + and *: bit-exact with
// the JS) in four independent chains k = 6+q stepping by z^4, four partial
// sums, then the soft clamp. Same operations and order as cellTailAt.
double CellTailAt(const Cell& c, double theta)
{
   const double C = jsm::cos(theta), S = jsm::sin(theta);
   const double c2 = C * C - S * S, s2 = C * S + S * C;
   const double c4 = c2 * c2 - s2 * s2, s4 = c2 * s2 + s2 * c2;
   double c0 = c4 * c2 - s4 * s2, s0 = c4 * s2 + s4 * c2;   // z^6
   double c1 = c0 * C - s0 * S, s1 = c0 * S + s0 * C;       // z^7
   double c8 = c4 * c4 - s4 * s4, s8 = c4 * s4 + s4 * c4;   // z^8
   double c3 = c8 * C - s8 * S, s3 = c8 * S + s8 * C;       // z^9
   const double* ac = c.tailAc;
   const double* as = c.tailAs;
   double t0 = 0, t1 = 0, t2 = 0, t3 = 0;
   for (int i = 0; i < N_TAIL; i += 4) {
      t0 += c0 * ac[i] - s0 * as[i];
      if (i + 1 < N_TAIL) t1 += c1 * ac[i + 1] - s1 * as[i + 1];
      if (i + 2 < N_TAIL) t2 += c8 * ac[i + 2] - s8 * as[i + 2];
      if (i + 3 < N_TAIL) t3 += c3 * ac[i + 3] - s3 * as[i + 3];
      double n = c0 * c4 - s0 * s4; s0 = c0 * s4 + s0 * c4; c0 = n;
      n = c1 * c4 - s1 * s4; s1 = c1 * s4 + s1 * c4; c1 = n;
      n = c8 * c4 - s8 * s4; s8 = c8 * s4 + s8 * c4; c8 = n;
      n = c3 * c4 - s3 * s4; s3 = c3 * s4 + s3 * c4; c3 = n;
   }
   const double t = ((t0 + t1) + t2) + t3;
   const double q = t / TAIL_MAX;
   return t / jsm::sqrt(1 + q * q);
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
   const int N = c.tailBound > 0 ? 256 : 32;   // the tail's harmonics go to k = 64
   const double cnr = cos(c.nucRot), snr = sin(c.nucRot);   // once, not per sample
   double neededFloor = CELL_MOD_MIN;
   for (int i = 0; i < N; i++) {
      const double th = ((double)i / N) * PI * 2;
      const double lx = cos(th) * (c.nucLong / 2), ly = sin(th) * (c.nucShort / 2);
      const double wx = c.nucOffX + lx * cnr - ly * snr;
      const double wy = c.nucOffY + lx * snr + ly * cnr;
      const double dist = hypot(wx, wy);
      const double angle = atan2(wy, wx);
      const double phi = angle - c.rot;
      const double base = (c.semiMajor * c.semiMinor) / hypot(c.semiMinor * cos(phi), c.semiMajor * sin(phi));
      if (base <= 1e-6) continue;
      double required = (dist * 1.05 + margin + extraUm) / base;
      if (c.tailBound > 0) required /= 1 + CellTailAt(c, angle);   // the tail at this angle
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
