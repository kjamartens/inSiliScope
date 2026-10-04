#include "cells.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace isc {

namespace {
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
}

void NormalizeParams(Params& p)
{
   p.cellDiamMax = std::max(p.cellDiamMin, p.cellDiamMax);
   p.cellElongMax = std::max(p.cellElongMin, p.cellElongMax);
   p.nucLongMax = std::max(p.nucLongMin, p.nucLongMax);
   p.nucRatioMax = std::max(p.nucRatioMin, p.nucRatioMax);
   p.nucHeightMax = std::max(p.nucHeightMin, p.nucHeightMax);
   p.nucBaseMax = std::max(p.nucBaseMin, p.nucBaseMax);
   p.nucIrregMax = std::max(p.nucIrregMin, p.nucIrregMax);
   p.nucBendMax = std::max(p.nucBendMin, p.nucBendMax);
   p.nucWidestMax = std::max(p.nucWidestMin, p.nucWidestMax);
   p.cytoRimHeightMax = std::max(p.cytoRimHeightMin, p.cytoRimHeightMax);
   p.cytoEdgeRiseMax = std::max(p.cytoEdgeRiseMin, p.cytoEdgeRiseMax);
   p.cytoMidHeightMax = std::max(p.cytoMidHeightMin, p.cytoMidHeightMax);
   p.cytoMidDistanceMax = std::max(p.cytoMidDistanceMin, p.cytoMidDistanceMax);
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

namespace {
void NucShapeInit(Cell& c, uint32_t seed, int32_t cx, int32_t cy, const Params& p);
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
   c.height = 0;   // the dome top: set by EnvelopNucleus from the nucleus (CH::HEIGHT is retired)
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
   // Gap between the coverslip and the nucleus bottom (NUC_ZFRAC channel);
   // EnvelopNucleus sets nucZ once the shape's extents are known.
   c.nucBase = std::max(0.0, lerp(p.nucBaseMin, p.nucBaseMax, H(CH::NUC_ZFRAC)));
   c.nucZ = 0;
   NucShapeInit(c, seed, cx, cy, p);

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

// Keeps the nucleus inside the cell with >= margin clearance. Vertically
// places the nucleus bottom nucBase above the coverslip and sets the dome top
// to the nucleus top + margin; laterally raises the per-cell modulation floor
// (not a whole-cell scale, see spec) and only scales the cell as a last resort.
void EnvelopNucleus(Cell& c, double marginUm, const Params* runoutParams)
{
   using namespace jsm;
   const double margin = std::max(0.1, marginUm);

   // up = down = nucHeight/2, or the shaped nucleus's own extents (NucShapeInit).
   const double up = c.nucUp, down = c.nucDown;
   c.height = c.nucBase + down + up + margin;
   c.nucZ = c.nucBase + down;

   // Evaluated after the vertical step: it needs the final c.height.
   const double extraUm = runoutParams ? CytoSlopeRunout(c, *runoutParams) : 0;
   // The tail's harmonics go to k = 64; a shaped nucleus walks its footprint polygon.
   const int N = c.nucShaped ? NUC_POLY_N : c.tailBound > 0 ? 256 : 32;
   std::vector<Pt2> poly;
   if (c.nucShaped) poly = NucFootprintPolygon(c);
   const double cnr = cos(c.nucRot), snr = sin(c.nucRot);   // once, not per sample
   double neededFloor = CELL_MOD_MIN;
   for (int i = 0; i < N; i++) {
      double wx, wy;
      if (c.nucShaped) {
         wx = poly[(size_t)i].x; wy = poly[(size_t)i].y;
      } else {
         const double th = ((double)i / N) * PI * 2;
         const double lx = cos(th) * (c.nucLong / 2), ly = sin(th) * (c.nucShort / 2);
         wx = c.nucOffX + lx * cnr - ly * snr;
         wy = c.nucOffY + lx * snr + ly * cnr;
      }
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


// ---- Nucleus shape -------------------------------------------------------
// Shape coordinates: footprint radius s (1 = the outline), direction t,
// height zeta in [-1, 1]:
//   (u, v) = s R(t) (cos t, sin t)          lobes, R = 1 + soft(sum_{k=2..8} Rc_k cos kt + Rs_k sin kt)
//   x = a u, y = b (v + bend (u^2 - 1/4))   kidney bend: a shear, exactly invertible
//   z = nucZ + rz k H zeta                  k = kDown (zeta < 0) or kUp, H = 1 + soft(sum_{k=1..8} s^k (...))
// in the nucleus frame (nucRot, nucOff); a, b, rz = nucLong/2, nucShort/2,
// nucHeight/2. Inside: |zeta| < 1 and s < W(zeta) = q + (1 - q) f,
// q = sqrt(1 - zeta^2), f = the widening of that half (nucFBot / nucFTop).
// cos kt / sin kt and (s e^{it})^k by complex multiplication (+ - * only), the
// spectrum by jsm::exp/log: bit-exact with the prototype. spec/ALGORITHM.md.
namespace {
const double SQRT3 = std::sqrt(3.0);

void NucSpectrum(int k0, double gamma, double w[NUC_K + 1])
{
   for (int k = 0; k <= NUC_K; k++) w[k] = 0;
   double sum = 0;
   for (int k = k0; k <= NUC_K; k++) { w[k] = jsm::exp(-gamma * jsm::log((double)k)); sum += w[k] * w[k]; }
   const double norm = std::sqrt(sum);
   for (int k = k0; k <= NUC_K; k++) w[k] /= norm;
}

// Draws the per-cell shape (after nucLong/Short/Height/Rot/Off are set).
// Draw order: irregularity, bend, widest point, Rc/Rs k = 2..8, Hc/Hs k = 1..8.
void NucShapeInit(Cell& c, uint32_t seed, int32_t cx, int32_t cy, const Params& p)
{
   HashStream next(seed, cx, cy, NUC_SHAPE_STREAM);
   const double irr = lerp(std::max(0.0, p.nucIrregMin), std::max(0.0, p.nucIrregMax), next.Next());
   c.nucBend = lerp(p.nucBendMin, p.nucBendMax, next.Next());
   // Widest section's height as a fraction of the nucleus height: the part
   // below it is 2w x rz tall, the part above 2(1 - w) x rz.
   const double wide = std::min(1.0, std::max(0.0, lerp(p.nucWidestMin, p.nucWidestMax, next.Next())));
   c.nucKDown = 2 * wide; c.nucKUp = 2 * (1 - wide);
   const double thick = std::max(0.0, p.nucThickIrreg);
   const double asym = std::isnan(p.nucAsym) ? 0 : std::min(NUC_ASYM_MAX, std::max(-NUC_ASYM_MAX, p.nucAsym));
   c.nucFBot = std::max(0.0, asym); c.nucFTop = std::max(0.0, -asym);
   c.nucShaped = irr > 0 || c.nucBend != 0 || thick > 0 || asym != 0 || wide != 0.5;
   c.nucUp = c.nucDown = c.nucHeight / 2;
   if (!c.nucShaped) return;
   const double gamma = std::max(0.0, p.nucSmooth);
   double wR[NUC_K + 1], wH[NUC_K + 1];
   NucSpectrum(2, gamma, wR);
   NucSpectrum(1, gamma, wH);
   for (int k = 2; k <= NUC_K; k++) {
      c.nucRc[k] = irr * wR[k] * (2 * next.Next() - 1) * SQRT3;
      c.nucRs[k] = irr * wR[k] * (2 * next.Next() - 1) * SQRT3;
   }
   for (int k = 1; k <= NUC_K; k++) {
      c.nucHc[k] = thick * wH[k] * (2 * next.Next() - 1) * SQRT3;
      c.nucHs[k] = thick * wH[k] * (2 * next.Next() - 1) * SQRT3;
   }
   c.nucCos = jsm::cos(c.nucRot); c.nucSin = jsm::sin(c.nucRot);
   // Vertical extents above/below nucZ: max of H x the column's extent over the footprint (sampled), x kUp/kDown.
   double up = 1, down = 1;
   if (thick > 0 || asym != 0) {
      up = 0; down = 0;
      for (int i = 0; i < 64; i++) {
         const double th = ((double)i / 64) * jsm::PI * 2, C = jsm::cos(th), S = jsm::sin(th);
         for (int j = 0; j < 16; j++) {
            const double sg = (double)j / 16, H = NucThickAt(c, sg, C, S);
            up = std::max(up, H * NucColumnExt(sg, c.nucFTop));
            down = std::max(down, H * NucColumnExt(sg, c.nucFBot));
         }
      }
   }
   c.nucUp = (c.nucHeight / 2) * c.nucKUp * up;
   c.nucDown = (c.nucHeight / 2) * c.nucKDown * down;
   // The farthest footprint point from the nucleus centre.
   c.nucReach = 0;
   for (const Pt2& q : NucFootprintPolygon(c))
      c.nucReach = std::max(c.nucReach, jsm::hypot(q.x - c.nucOffX, q.y - c.nucOffY));
}
} // namespace

double NucFootR(const Cell& c, double C, double S)
{
   double pc = C, ps = S, t = 0;
   for (int k = 2; k <= NUC_K; k++) {
      const double n = pc * C - ps * S; ps = pc * S + ps * C; pc = n;
      t += c.nucRc[k] * pc + c.nucRs[k] * ps;
   }
   const double q = t / NUC_R_MAX;
   return 1 + t / std::sqrt(1 + q * q);
}

double NucThickAt(const Cell& c, double sg, double C, double S)
{
   const double wr = sg * C, wi = sg * S;
   double pc = 1, ps = 0, t = 0;
   for (int k = 1; k <= NUC_K; k++) {
      const double n = pc * wr - ps * wi; ps = pc * wi + ps * wr; pc = n;
      t += c.nucHc[k] * pc + c.nucHs[k] * ps;
   }
   const double q = t / NUC_H_MAX;
   return 1 + t / std::sqrt(1 + q * q);
}

double NucSectionW(double zeta, double f)
{
   const double q = std::sqrt(std::max(0.0, 1 - zeta * zeta));
   return q + (1 - q) * f;
}

double NucColumnExt(double sg, double f)
{
   if (sg <= f) return 1;
   const double qs = (sg - f) / (1 - f);
   return std::sqrt(std::max(0.0, 1 - qs * qs));
}

Pt3 NucMapLocal(const Cell& c, double sg, double C, double S, double zeta)
{
   const double a = c.nucLong / 2, b = c.nucShort / 2;
   const double R = NucFootR(c, C, S);
   const double u = sg * R * C, v = sg * R * S;
   const double x = a * u, y = b * (v + c.nucBend * (u * u - 0.25));
   return { c.nucOffX + x * c.nucCos - y * c.nucSin, c.nucOffY + x * c.nucSin + y * c.nucCos,
            c.nucZ + (c.nucHeight / 2) * (zeta < 0 ? c.nucKDown : c.nucKUp) * NucThickAt(c, sg, C, S) * zeta };
}

NucBall NucBallLocal(const Cell& c, double lx0, double ly0)
{
   const double dx = lx0 - c.nucOffX, dy = ly0 - c.nucOffY;
   const double x = dx * c.nucCos + dy * c.nucSin, y = -dx * c.nucSin + dy * c.nucCos;
   const double u = x / std::max(1e-6, c.nucLong / 2);
   const double v = y / std::max(1e-6, c.nucShort / 2) - c.nucBend * (u * u - 0.25);
   const double rho = std::sqrt(u * u + v * v);
   const double C = rho > 1e-12 ? u / rho : 1, S = rho > 1e-12 ? v / rho : 0;
   const double s = rho / NucFootR(c, C, S);
   return { s, C, S, NucThickAt(c, s, C, S) };
}

bool NucleusColumnLocal(const Cell& c, double lx, double ly, double& below, double& above)
{
   if (!c.nucShaped) {
      const double ncr = jsm::cos(-c.nucRot), nsr = jsm::sin(-c.nucRot);
      const double na = std::max(1e-6, c.nucLong / 2), nb = std::max(1e-6, c.nucShort / 2);
      const double ex = lx - c.nucOffX, ey = ly - c.nucOffY;
      const double ux = (ex * ncr - ey * nsr) / na, uy = (ex * nsr + ey * ncr) / nb;
      const double q = 1 - ux * ux - uy * uy;
      if (!(q > 0)) return false;
      below = above = (c.nucHeight / 2) * std::sqrt(q);
      return true;
   }
   const NucBall B = NucBallLocal(c, lx, ly);
   if (!(B.s < 1)) return false;
   const double rzH = (c.nucHeight / 2) * B.H;
   below = rzH * c.nucKDown * NucColumnExt(B.s, c.nucFBot);
   above = rzH * c.nucKUp * NucColumnExt(B.s, c.nucFTop);
   return true;
}

bool NucPushOutLocal(const Cell& c, Pt3& pt, double margin)
{
   const NucBall B = NucBallLocal(c, pt.x, pt.y);
   const double zeta = (pt.z - c.nucZ) / std::max(1e-6, (c.nucHeight / 2) * B.H * (pt.z < c.nucZ ? c.nucKDown : c.nucKUp));
   auto inside = [&](double sg, double z) {
      return std::fabs(z) < 1 && sg < NucSectionW(z, z < 0 ? c.nucFBot : c.nucFTop);
   };
   if (!inside(B.s, zeta)) return false;
   const double m = std::max(B.s, std::fabs(zeta));
   if (!(m > 1e-9)) return false;   // the very centre: no direction (as the ellipsoid path)
   double lo = 1, hi = 1 / m;       // inside at lo, outside (or on the boundary) at hi
   for (int it = 0; it < NUC_PUSH_ITERS; it++) {
      const double mid = (lo + hi) / 2;
      if (inside(B.s * mid, zeta * mid)) lo = mid; else hi = mid;
   }
   const double lam = hi / margin;
   pt = NucMapLocal(c, B.s * lam, B.C, B.S, zeta * lam);
   return true;
}

std::vector<Pt2> NucFootprintPolygon(const Cell& c)
{
   std::vector<Pt2> poly((size_t)NUC_POLY_N);
   for (int i = 0; i < NUC_POLY_N; i++) {
      const double th = ((double)i / NUC_POLY_N) * jsm::PI * 2;
      const Pt3 q = NucMapLocal(c, 1, jsm::cos(th), jsm::sin(th), 0);
      poly[(size_t)i] = { q.x, q.y };
   }
   return poly;
}

std::vector<Pt3> NucleusRingsLocal(const Cell& c, int slices, int pts)
{
   std::vector<Pt3> out;
   out.reserve((size_t)slices * pts);
   const double cr = jsm::cos(c.nucRot), sr = jsm::sin(c.nucRot);
   for (int sl = 0; sl < slices; sl++) {
      const double zeta = -jsm::cos(jsm::PI * sl / (slices - 1));
      const double s = c.nucShaped ? NucSectionW(zeta, zeta < 0 ? c.nucFBot : c.nucFTop)
                                   : std::sqrt(std::max(0.0, 1 - zeta * zeta));
      for (int i = 0; i < pts; i++) {
         const double th = ((double)i / pts) * jsm::PI * 2, C = jsm::cos(th), S = jsm::sin(th);
         if (!c.nucShaped) {
            const double lx = C * (c.nucLong / 2) * s, ly = S * (c.nucShort / 2) * s;
            out.push_back({ c.nucOffX + lx * cr - ly * sr, c.nucOffY + lx * sr + ly * cr, c.nucZ + zeta * c.nucHeight / 2 });
         } else {
            out.push_back(NucMapLocal(c, s, C, S, zeta));
         }
      }
   }
   return out;
}

} // namespace isc
