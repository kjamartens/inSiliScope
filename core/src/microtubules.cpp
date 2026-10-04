#include "microtubules.h"

#include "parallel.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace isc {

namespace {

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

double Smoothstep(double t)
{
   t = std::max(0.0, std::min(1.0, t));
   return t * t * (3 - 2 * t);
}

// JS ToInt32 of an integral double (Math.imul's argument conversion).
inline uint32_t ToU32(double d) { return (uint32_t)(int64_t)d; }

struct Pt2m { double x, y; };

double MtNucleusClearance(const Params& p) { return std::max(0.05, 0.5 * p.nucMargin); }

// Moving average of every interior point (endpoints left alone), window
// i-h..i+h, h = min(half, i, n-1-i) (symmetric: it shrinks toward the ends).
// Prefix sums in index order, as mtBoxSmooth.
template <class Set>
void MtBoxSmooth(const std::vector<double>& vals, long half, Set set)
{
   const long n = (long)vals.size();
   if (n < 3 || !(half > 0)) return;
   std::vector<double> cum((size_t)n + 1);
   cum[0] = 0;
   for (long i = 0; i < n; i++) cum[(size_t)i + 1] = cum[(size_t)i] + vals[(size_t)i];
   for (long i = 1; i < n - 1; i++) {
      const long h = std::min(half, std::min(i, n - 1 - i));
      set((size_t)i, (cum[(size_t)(i + h + 1)] - cum[(size_t)(i - h)]) / (double)(2 * h + 1));
   }
}

// ---- Obstacles a path rides over or under (mtObstacles) ----
// Anything a microtubule must not pass through and gets around by going over
// or under it, one z interval per (x, y) column. The nucleus is the only one
// so far; a new structure adds a subclass to the list in MtGenerateOne (list
// order matters: the bounds loop over it in order).
struct MtObstacle {
   double clearance = 0;
   virtual ~MtObstacle() = default;
   // [bottom, top] (absolute z, cell-local x/y) of the obstacle over this point; false if none.
   virtual bool Column(double x, double y, double& bottom, double& top) const = 0;
   // Over (true) or under, decided once per path from its start height.
   virtual bool GoOver(double startZ) const = 0;
};

struct MtNucleusObstacle final : MtObstacle {
   const Cell& c;
   double bottomZ;
   MtNucleusObstacle(const Cell& cell, const Params& p) : c(cell), bottomZ(cell.nucZ - cell.nucDown)
   {
      clearance = MtNucleusClearance(p);
   }
   bool Column(double x, double y, double& bottom, double& top) const override
   {
      double below, above;
      if (!NucleusColumnLocal(c, x, y, below, above)) return false;
      bottom = c.nucZ - below; top = c.nucZ + above;
      return true;
   }
   // Over when the path starts above the widest section, or there is no room below.
   bool GoOver(double startZ) const override { return bottomZ - clearance <= 0 || startZ >= c.nucZ; }
};

// mtObstacleBounds: hi = ceiling x margin; per obstacle over point i, lo up to
// top + clearance (over) or hi down to bottom - clearance (under, at most half
// the room below); the clearance fades in over MT_OBST_CLEAR_RAMP_UM of xy arc.
void MtObstacleBounds(const std::vector<Pt3>& pts, const std::vector<double>& ceil,
                      const std::vector<const MtObstacle*>& obstacles, std::vector<double>& lo, std::vector<double>& hi)
{
   const size_t n = pts.size();
   std::vector<char> over(obstacles.size());
   for (size_t k = 0; k < obstacles.size(); k++) over[k] = obstacles[k]->GoOver(pts[0].z);
   lo.assign(n, -std::numeric_limits<double>::infinity());
   hi.assign(n, 0.0);
   double arc = 0;
   for (size_t i = 0; i < n; i++) {
      if (i > 0) arc += jsm::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
      hi[i] = ceil[i] * MT_CONTAIN_MARGIN;
      const double fade = std::min(1.0, arc / MT_OBST_CLEAR_RAMP_UM);
      for (size_t k = 0; k < obstacles.size(); k++) {
         double bottom, top;
         if (!obstacles[k]->Column(pts[i].x, pts[i].y, bottom, top)) continue;
         const double clr = obstacles[k]->clearance * fade;
         if (over[k]) lo[i] = std::max(lo[i], top + clr);
         else hi[i] = std::min(hi[i], bottom - std::min(clr, 0.5 * bottom));
      }
   }
   for (size_t i = 0; i < n; i++) lo[i] = std::min(lo[i], hi[i]);
}

// mtObstacleEnvelope: keeps interior z within [lo, hi] by smooth corrections:
// the shortfall spread into a ramp of slope MT_OBST_RAMP_SLOPE per um in xy
// (forward and backward running maxima), box-smoothed over `half` points, then
// raised back to the shortfall. Lift first (lo), then lower (hi). Endpoints
// never move.
void MtObstacleEnvelope(std::vector<Pt3>& pts, const std::vector<double>& lo, const std::vector<double>& hi, long half)
{
   const size_t n = pts.size();
   if (n < 3) return;
   std::vector<double> ds(n, 0.0);
   for (size_t i = 1; i < n; i++) ds[i] = jsm::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
   std::vector<double> need(n), ramp(n), corr(n);
   for (const int sign : { 1, -1 }) {
      bool any = false;
      for (size_t i = 0; i < n; i++) {
         need[i] = i == 0 || i == n - 1 ? 0 : std::max(0.0, sign > 0 ? lo[i] - pts[i].z : pts[i].z - hi[i]);
         if (need[i] > 0) any = true;
      }
      if (!any) continue;
      ramp[0] = need[0];
      for (size_t i = 1; i < n; i++) ramp[i] = std::max(need[i], ramp[i - 1] - MT_OBST_RAMP_SLOPE * ds[i]);
      for (size_t i = n - 1; i-- > 0;) ramp[i] = std::max(ramp[i], ramp[i + 1] - MT_OBST_RAMP_SLOPE * ds[i + 1]);
      corr = ramp;
      MtBoxSmooth(ramp, half, [&](size_t i, double v) { corr[i] = std::max(v, need[i]); });
      for (size_t i = 1; i + 1 < n; i++) pts[i].z += sign * corr[i];
   }
}

void MtEnforceMinTurnRadius(std::vector<Pt2m>& pts, double minRadius)
{
   const size_t n = pts.size();
   if (!(minRadius > 0) || n < 3) return;
   const double endX = pts[n - 1].x, endY = pts[n - 1].y;
   std::vector<double> segX(n - 1), segY(n - 1), segLen(n - 1);
   for (int round = 0; round < 8; round++) {
      for (size_t i = 1; i < n; i++) {
         segX[i - 1] = pts[i].x - pts[i - 1].x;
         segY[i - 1] = pts[i].y - pts[i - 1].y;
         segLen[i - 1] = jsm::hypot(segX[i - 1], segY[i - 1]);
      }
      bool changed = false;
      for (size_t i = 1; i < n - 1; i++) {
         const double prevLen = segLen[i - 1], curLen = segLen[i];
         if (prevLen < 1e-9 || curLen < 1e-9) continue;
         const double prevAng = jsm::atan2(segY[i - 1], segX[i - 1]);
         const double curAng = jsm::atan2(segY[i], segX[i]);
         double dAng = curAng - prevAng;
         while (dAng > jsm::PI) dAng -= 2 * jsm::PI;
         while (dAng < -jsm::PI) dAng += 2 * jsm::PI;
         const double avgLen = (prevLen + curLen) / 2;
         const double cap = 2 * jsm::asin(std::min(1.0, avgLen / (2 * minRadius)));
         if (std::fabs(dAng) <= cap) continue;
         const double clampedAng = prevAng + (dAng > 0 ? 1 : -1) * cap;
         segX[i] = jsm::cos(clampedAng) * curLen;
         segY[i] = jsm::sin(clampedAng) * curLen;
         changed = true;
      }
      for (size_t i = 1; i < n; i++) {
         pts[i].x = pts[i - 1].x + segX[i - 1];
         pts[i].y = pts[i - 1].y + segY[i - 1];
      }
      const double dxErr = endX - pts[n - 1].x, dyErr = endY - pts[n - 1].y;
      if (std::fabs(dxErr) > 1e-9 || std::fabs(dyErr) > 1e-9) {
         for (size_t i = 1; i < n - 1; i++) {
            const double t = (double)i / (n - 1);
            const double s = t * t * (3 - 2 * t);
            pts[i].x += dxErr * s;
            pts[i].y += dyErr * s;
         }
         pts[n - 1].x = endX; pts[n - 1].y = endY;
      }
      if (!changed) break;
   }
}

void MtTrimSteepEnds(std::vector<Pt3>& pts, double maxSlope)
{
   if (!(maxSlope > 0)) return;
   size_t front = 0;
   for (int k = 0; k < MT_MAX_END_TRIM && pts.size() - front > 2; k++) {
      const Pt3& a = pts[front]; const Pt3& b = pts[front + 1];
      const double lateral = jsm::hypot(b.x - a.x, b.y - a.y);
      if (std::fabs(b.z - a.z) <= maxSlope * std::max(lateral, 1e-6)) break;
      front++;
   }
   pts.erase(pts.begin(), pts.begin() + front);
   for (int k = 0; k < MT_MAX_END_TRIM && pts.size() > 2; k++) {
      const size_t last = pts.size() - 1;
      const Pt3& a = pts[last]; const Pt3& b = pts[last - 1];
      const double lateral = jsm::hypot(a.x - b.x, a.y - b.y);
      if (std::fabs(a.z - b.z) <= maxSlope * std::max(lateral, 1e-6)) break;
      pts.pop_back();
   }
}

void MtLimitZSlopeRealized(std::vector<Pt3>& pts, double maxSlope)
{
   const size_t n = pts.size();
   if (!(maxSlope > 0) || n < 3) return;
   for (int pass = 0; pass < 20; pass++) {
      bool changed = false;
      const bool forward = pass % 2 == 0;
      for (size_t k = 1; k < n - 1; k++) {
         const size_t i = forward ? k : n - 1 - k;
         const Pt3& prev = pts[i - 1]; Pt3& cur = pts[i]; const Pt3& nxt = pts[i + 1];
         const double dPrev = jsm::hypot(cur.x - prev.x, cur.y - prev.y) * maxSlope;
         const double dNxt = jsm::hypot(nxt.x - cur.x, nxt.y - cur.y) * maxSlope;
         const double lo = std::max(prev.z - dPrev, nxt.z - dNxt);
         const double hi = std::min(prev.z + dPrev, nxt.z + dNxt);
         if (lo > hi) continue;
         if (cur.z < lo) { cur.z = lo; changed = true; }
         else if (cur.z > hi) { cur.z = hi; changed = true; }
      }
      if (!changed) break;
   }
}

// JS `Math.hypot(...) || 1`: 0 and NaN both become 1.
inline double NormOr1(double n) { return (n == 0 || std::isnan(n)) ? 1 : n; }

double MtShoelaceArea(const std::vector<Pt2>& pts)
{
   double area = 0;
   const size_t n = pts.size();
   for (size_t i = 0; i < n; i++) {
      const Pt2& a = pts[i]; const Pt2& b = pts[(i + 1) % n];
      area += a.x * b.y - b.x * a.y;
   }
   return std::fabs(area) / 2;
}

// Approximate distance (um) from cell-local (x, y, z) to the nucleus surface,
// or -1 inside (mtNucleusGap): in the nucleus shape coordinates, the lateral
// gap to the section at this height and the vertical gap to the column,
// combined as gl gv / hypot(gl, gv). The plain ellipsoid is f = 0, H = 1.
double MtNucleusGap(const Cell& c, double x, double y, double z)
{
   using namespace jsm;
   const bool up = z >= c.nucZ;
   double s, rDir, ext, f;
   if (c.nucShaped) {
      const NucBall B = NucBallLocal(c, x, y);
      s = B.s;
      const Pt3 q = NucMapLocal(c, 1, B.C, B.S, 0);
      rDir = hypot(q.x - c.nucOffX, q.y - c.nucOffY);
      ext = (c.nucHeight / 2) * (up ? c.nucKUp : c.nucKDown) * (s < 1 ? B.H : NucThickAt(c, 1, B.C, B.S));
      f = up ? c.nucFTop : c.nucFBot;
   } else {
      const double a = std::max(1e-6, c.nucLong / 2), b = std::max(1e-6, c.nucShort / 2);
      const double dx = x - c.nucOffX, dy = y - c.nucOffY;
      const double cr = cos(c.nucRot), sr = sin(c.nucRot);
      const double u = (dx * cr + dy * sr) / a, v = (-dx * sr + dy * cr) / b;
      s = hypot(u, v);
      const double C = s > 1e-12 ? u / s : 1, S = s > 1e-12 ? v / s : 0;
      rDir = hypot(a * C, b * S);
      ext = c.nucHeight / 2;
      f = 0;
   }
   const double zeta = std::fabs(z - c.nucZ) / std::max(1e-6, ext);
   if (zeta < 1) {
      const double W = NucSectionW(zeta, f);
      if (s < W) return -1;
      const double gl = (s - W) * rDir;
      if (s >= 1) return gl;
      const double gv = (zeta - NucColumnExt(s, f)) * ext;
      const double h = hypot(gl, gv);
      return h > 1e-12 ? gl * gv / h : 0;
   }
   if (s < 1) return (zeta - NucColumnExt(s, f)) * ext;
   return std::min(hypot((s - 1) * rDir, zeta * ext), hypot((s - f) * rDir, (zeta - 1) * ext));
}

// Inside the cytoplasm volume (footprint, ceiling, coverslip) with the containment margin.
bool MtInCytoplasm(const Cell& c, const MtCellGeom& g, double x, double y, double z)
{
   if (!(z > 0)) return false;
   if (!(jsm::hypot(x, y) < CellRadiusAt(c, jsm::atan2(y, x)) * MT_CONTAIN_MARGIN)) return false;
   return z < std::max(0.0, SampleCytoMeshHeight(c, g.mesh, x, y)) * MT_CONTAIN_MARGIN;
}

// Decay length: pct % of the cell's equivalent diameter, floored.
double MtDecayLen(double pct, const MtCellGeom& g) { return std::max(MT_MIN_DECAY_UM, pct / 100 * g.sizeUm); }

// START: a cytoplasm point with density ~ exp(-gap to the nucleus / lambda):
// uniform candidates in the nucleus box grown by MT_DECAY_SPAN lambda, four
// draws each (x, y, z, u), always; the closest valid rejected one is the
// fallback.
Pt3 MtSampleStart(const Cell& c, const Params& p, const MtCellGeom& g, HashStream& next)
{
   const double lam = MtDecayLen(p.mtStartDecayPct, g);
   const double L = std::min(MT_DECAY_SPAN * lam, g.rMax);
   const double* nb = g.nucBox;
   const double x0 = nb[0] - L, x1 = nb[2] + L, y0 = nb[1] - L, y1 = nb[3] + L, z1 = c.nucZ + c.nucUp + L;
   bool haveBest = false;
   Pt3 best{ 0, 0, 0 };
   double bestD = std::numeric_limits<double>::infinity();
   for (int t = 0; t < MT_SAMPLE_TRIES; t++) {
      const double x = lerp(x0, x1, next.Next());
      const double y = lerp(y0, y1, next.Next());
      const double z = z1 * next.Next();
      const double u = next.Next();
      const double d = MtNucleusGap(c, x, y, z);
      if (d < 0) continue;
      const bool accept = u < jsm::exp(-d / lam);
      if (!accept && !(d < bestD)) continue;
      if (!MtInCytoplasm(c, g, x, y, z)) continue;
      if (accept) return { x, y, z };
      best = { x, y, z }; bestD = d; haveBest = true;
   }
   if (haveBest) return best;
   // Nothing inside the cytoplasm (a degenerate cell): just outside the nucleus rim.
   const double cr = jsm::cos(c.nucRot), sr = jsm::sin(c.nucRot), r = c.nucLong / 2 / MT_CONTAIN_MARGIN;
   return { c.nucOffX + r * cr, c.nucOffY + r * sr, c.nucZ };
}

// END (x, y): a footprint point outside the nucleus footprint, density ~
// exp(-gap to the outline / lambda); polar proposal, three draws each.
Pt2m MtSampleEnd(const Cell& c, const Params& p, const MtCellGeom& g, HashStream& next)
{
   const double lam = MtDecayLen(p.mtEndDecayPct, g);
   double x = 0, y = 0;
   for (int t = 0; t < MT_SAMPLE_TRIES; t++) {
      const double th = next.Next() * jsm::PI * 2;
      const double d = -lam * jsm::log(1 - next.Next());
      const double u = next.Next();
      const double rc = CellRadiusAt(c, th) * MT_CONTAIN_MARGIN;
      const double r = rc - d;
      x = jsm::cos(th) * std::max(0.0, r); y = jsm::sin(th) * std::max(0.0, r);
      if (!(r > 0) || !(u * g.rMax < r)) continue;
      double below, above;
      if (NucleusColumnLocal(c, x, y, below, above)) continue;
      return { x, y };
   }
   return { x, y };
}

// The end for start S: MT_END_CANDIDATES ends, one picked with weight
// exp(mtDirKappa (cos a - 1)), a = angle between S -> end and centre -> S.
Pt2m MtPickEnd(const Cell& c, const Params& p, const MtCellGeom& g, HashStream& next, const Pt3& S)
{
   const double kappa = std::max(0.0, p.mtDirKappa);
   const double ox = S.x - c.nucOffX, oy = S.y - c.nucOffY, oL = jsm::hypot(ox, oy);
   Pt2m cands[MT_END_CANDIDATES];
   double w[MT_END_CANDIDATES];
   double sum = 0;
   for (int k = 0; k < MT_END_CANDIDATES; k++) {
      const Pt2m E = MtSampleEnd(c, p, g, next);
      const double ex = E.x - S.x, ey = E.y - S.y, eL = jsm::hypot(ex, ey);
      const double cs = oL > 1e-9 && eL > 1e-9 ? (ex * ox + ey * oy) / (eL * oL) : 0;
      cands[k] = E;
      w[k] = jsm::exp(kappa * (cs - 1));
      sum += w[k];
   }
   const double target = next.Next() * sum;
   double acc = 0;
   for (int k = 0; k < MT_END_CANDIDATES; k++) {
      acc += w[k];
      if (target < acc) return cands[k];
   }
   return cands[MT_END_CANDIDATES - 1];
}

// knownInside: the caller has just checked this point against the outline
// (the cut below); skip that check again unless the nucleus push moves it.
void MtClampIntoCytoplasm(const Cell& c, const MtCellGeom& g, Pt3& pt, bool knownInside = false)
{
   if (c.nucShaped) {
      // The same radial push in the shaped nucleus's own coordinates.
      if (NucPushOutLocal(c, pt, MT_CONTAIN_MARGIN)) knownInside = false;
   } else {
   const double dxN = pt.x - c.nucOffX, dyN = pt.y - c.nucOffY;
   const double cr = g.nucCosNeg, sr = g.nucSinNeg;
   const double lx = dxN * cr - dyN * sr, ly = dxN * sr + dyN * cr;
   const double a = c.nucLong / 2, b = c.nucShort / 2, rz = c.nucHeight / 2;
   const double nz = (pt.z - c.nucZ) / std::max(1e-6, rz);
   const double nx = lx / std::max(1e-6, a), ny = ly / std::max(1e-6, b);
   const double ellNorm = jsm::sqrt(nx * nx + ny * ny + nz * nz);
   if (ellNorm < 1) {
      const double scale = (ellNorm > 1e-9 ? 1 / ellNorm : 1) / MT_CONTAIN_MARGIN;
      const double lx2 = lx * scale, ly2 = ly * scale;
      pt.z = c.nucZ + nz * rz * scale;
      const double cr2 = g.nucCosPos, sr2 = g.nucSinPos;
      pt.x = c.nucOffX + lx2 * cr2 - ly2 * sr2;
      pt.y = c.nucOffY + lx2 * sr2 + ly2 * cr2;
      knownInside = false;
   }
   }
   const double dist = jsm::hypot(pt.x, pt.y);
   const double rc = knownInside || dist <= CellInnerRadiusBound(c) ? dist : CellRadiusAt(c, jsm::atan2(pt.y, pt.x));
   if (dist > rc) {
      const double scale = (rc * MT_CONTAIN_MARGIN) / std::max(1e-9, dist);
      pt.x *= scale; pt.y *= scale;
   }
   const double topH = std::max(0.0, SampleCytoMeshHeight(c, g.mesh, pt.x, pt.y));
   if (pt.z < 0) pt.z = 0;
   else if (pt.z > topH) pt.z = topH * MT_CONTAIN_MARGIN;
}

// ---- collision resolution ----
inline uint32_t MtBucketKey(uint32_t ix, uint32_t iy, uint32_t iz)
{
   return (ix * 73856093u) ^ (iy * 19349663u) ^ (iz * 83492791u);
}

// Same hash buckets as the JS Map<number, [i, j, ...]>, including its
// behaviour on key collisions (buckets sharing a key share one list).
using SpatialIndex = std::unordered_map<uint32_t, std::vector<int32_t>>;

SpatialIndex MtBuildSpatialIndex(const std::vector<Microtubule>& mts, double cellSize)
{
   SpatialIndex grid;
   for (size_t i = 0; i < mts.size(); i++) {
      const std::vector<Pt3>& pts = mts[i].pts;
      for (size_t j = 0; j < pts.size(); j++) {
         const Pt3& pt = pts[j];
         const uint32_t key = MtBucketKey(ToU32(std::floor(pt.x / cellSize)), ToU32(std::floor(pt.y / cellSize)),
                                          ToU32(std::floor(pt.z / cellSize)));
         std::vector<int32_t>& arr = grid[key];
         arr.push_back((int32_t)i);
         arr.push_back((int32_t)j);
      }
   }
   return grid;
}

bool MtNudgeRoundGrid(const Cell& c, const MtCellGeom& g, std::vector<Microtubule>& mts, double minSep)
{
   const double cellSize = std::max(minSep, 1e-6);
   const SpatialIndex grid = MtBuildSpatialIndex(mts, cellSize);
   std::vector<std::vector<Pt3>> pushes(mts.size());
   for (size_t i = 0; i < mts.size(); i++) pushes[i].assign(mts[i].pts.size(), Pt3{ 0, 0, 0 });
   bool any = false;
   long ops = 0;
   bool stop = false;
   for (size_t i = 0; i < mts.size() && !stop; i++) {
      const std::vector<Pt3>& pts = mts[i].pts;
      for (size_t j = 1; j + 1 < pts.size() && !stop; j++) {
         const Pt3& pt = pts[j];
         const double ix = std::floor(pt.x / cellSize), iy = std::floor(pt.y / cellSize), iz = std::floor(pt.z / cellSize);
         for (int dx = -1; dx <= 1 && !stop; dx++) for (int dy = -1; dy <= 1 && !stop; dy++) for (int dz = -1; dz <= 1 && !stop; dz++) {
            auto it = grid.find(MtBucketKey(ToU32(ix + dx), ToU32(iy + dy), ToU32(iz + dz)));
            if (it == grid.end()) continue;
            const std::vector<int32_t>& arr = it->second;
            for (size_t k = 0; k < arr.size(); k += 2) {
               const int32_t i2 = arr[k], j2 = arr[k + 1];
               if ((size_t)i2 == i) continue;
               if (++ops > MT_COLLISION_OP_BUDGET) { stop = true; break; }
               const Pt3& p2 = mts[i2].pts[j2];
               const double ddx = pt.x - p2.x, ddy = pt.y - p2.y, ddz = pt.z - p2.z;
               const double d = jsm::hypot(ddx, ddy, ddz);
               if (d >= minSep) continue;
               any = true;
               double nx, ny, nz;
               if (d < 1e-6) { nx = 1; ny = 0; nz = 0; } else { nx = ddx / d; ny = ddy / d; nz = ddz / d; }
               const double mag = (minSep - d) / 2 + 1e-4;
               Pt3& push = pushes[i][j];
               push.x += nx * mag; push.y += ny * mag; push.z += nz * mag;
            }
         }
      }
   }
   if (!any) return false;
   for (size_t i = 0; i < mts.size(); i++) {
      std::vector<Pt3>& pts = mts[i].pts;
      for (size_t j = 1; j + 1 < pts.size(); j++) {
         const Pt3& push = pushes[i][j];
         if (push.x == 0 && push.y == 0 && push.z == 0) continue;
         pts[j].x += push.x; pts[j].y += push.y; pts[j].z += push.z;
         MtClampIntoCytoplasm(c, g, pts[j]);
      }
   }
   return true;
}

void MtResolveCollisions(uint32_t seed, const Cell& c, const Params& p, std::vector<Microtubule>& mts, const MtCellGeom& g)
{
   const double minSep = std::max(0.0, p.mtMinSeparation);
   if (minSep <= 0 || mts.size() < 2) return;
   size_t totalPoints = 0;
   for (const Microtubule& m : mts) totalPoints += m.pts.size();
   if (totalPoints > MT_COLLISION_MAX_TOTAL_POINTS) return;

   auto nudgePass = [&](int rounds) {
      for (int round = 0; round < rounds; round++)
         if (!MtNudgeRoundGrid(c, g, mts, minSep)) return true;
      return false;
   };
   if (nudgePass(MT_NUDGE_ROUNDS)) return;

   for (int resampleRound = 0; resampleRound < MT_RESAMPLE_ROUNDS; resampleRound++) {
      const double cellSize = std::max(minSep, 1e-6);
      const SpatialIndex grid = MtBuildSpatialIndex(mts, cellSize);
      std::vector<int> violating;   // ascending = JS Set insertion order
      for (size_t i = 0; i < mts.size(); i++) {
         const std::vector<Pt3>& pts = mts[i].pts;
         bool found = false;
         for (size_t j = 0; j < pts.size() && !found; j++) {
            const Pt3& pt = pts[j];
            const double ix = std::floor(pt.x / cellSize), iy = std::floor(pt.y / cellSize), iz = std::floor(pt.z / cellSize);
            for (int dx = -1; dx <= 1 && !found; dx++) for (int dy = -1; dy <= 1 && !found; dy++) for (int dz = -1; dz <= 1 && !found; dz++) {
               auto it = grid.find(MtBucketKey(ToU32(ix + dx), ToU32(iy + dy), ToU32(iz + dz)));
               if (it == grid.end()) continue;
               const std::vector<int32_t>& arr = it->second;
               for (size_t k = 0; k < arr.size(); k += 2) {
                  const int32_t i2 = arr[k], j2 = arr[k + 1];
                  if ((size_t)i2 == i) continue;
                  const Pt3& p2 = mts[i2].pts[j2];
                  if (jsm::hypot(pt.x - p2.x, pt.y - p2.y, pt.z - p2.z) < minSep) { found = true; break; }
               }
            }
         }
         if (found) violating.push_back((int)i);
      }
      if (violating.empty()) return;
      // Array.prototype.sort is stable in V8.
      std::stable_sort(violating.begin(), violating.end(),
                       [&](int a, int b) { return mts[a].priority < mts[b].priority; });
      const size_t loserCount = std::max<size_t>(1, (violating.size() + 1) / 2);
      for (size_t k = 0; k < loserCount; k++) {
         const int idx = violating[k];
         mts[idx] = MtGenerateOne(seed, idx, resampleRound + 1, c, p, g);
      }
      if (nudgePass(MT_POST_RESAMPLE_NUDGE_ROUNDS)) return;
   }
}

} // namespace

MtCellGeom BuildMtCellGeom(const Cell& c, const Params& p)
{
   MtCellGeom g;
   const std::vector<Pt2> outline = CellOutlineLocal(c, 48);
   g.areaUm2 = MtShoelaceArea(outline);
   g.sizeUm = 2 * std::sqrt(g.areaUm2 / jsm::PI);   // equivalent diameter
   double rMax = 0;
   for (int i = 0; i < MT_RMAX_SAMPLES; i++) rMax = std::max(rMax, CellRadiusAt(c, ((double)i / MT_RMAX_SAMPLES) * jsm::PI * 2));
   g.rMax = rMax * MT_CONTAIN_MARGIN;
   if (c.nucShaped) {
      double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
      for (const Pt2& q : NucFootprintPolygon(c)) {
         x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y);
      }
      g.nucBox[0] = x0; g.nucBox[1] = y0; g.nucBox[2] = x1; g.nucBox[3] = y1;
   } else {
      const double a = c.nucLong / 2, b = c.nucShort / 2, cr = jsm::cos(c.nucRot), sr = jsm::sin(c.nucRot);
      const double hx = jsm::hypot(a * cr, b * sr), hy = jsm::hypot(a * sr, b * cr);
      g.nucBox[0] = c.nucOffX - hx; g.nucBox[1] = c.nucOffY - hy; g.nucBox[2] = c.nucOffX + hx; g.nucBox[3] = c.nucOffY + hy;
   }
   g.mesh = BuildCytoMesh(c, p);
   g.nucCosNeg = jsm::cos(-c.nucRot);
   g.nucSinNeg = jsm::sin(-c.nucRot);
   g.nucCosPos = jsm::cos(c.nucRot);
   g.nucSinPos = jsm::sin(c.nucRot);
   return g;
}

int MtCountForCell(uint32_t seed, const Cell& c, const Params& p, const MtCellGeom& g)
{
   const double expected = std::max(0.0, g.areaUm2 * p.mtDensity);
   const double base = std::floor(expected);
   const double frac = expected - base;
   const double roll = HashUnit(seed, c.cx, c.cy, MT_CH_COUNT);
   return (int)base + (roll < frac ? 1 : 0);
}

Microtubule MtGenerateOne(uint32_t seed, int mtIndex, int resampleRound, const Cell& c, const Params& p, const MtCellGeom& g)
{
   using namespace jsm;
   HashStream next(seed, c.cx, c.cy, MT_STREAM_BASE + (uint32_t)mtIndex + (uint32_t)resampleRound * MT_RESAMPLE_SPACING);

   const Pt3 S = MtSampleStart(c, p, g, next);
   const Pt2m E = MtPickEnd(c, p, g, next, S);
   const double startX = S.x, startY = S.y, endX = E.x, endY = E.y;

   const double startTopH = std::max(0.0, SampleCytoMeshHeight(c, g.mesh, startX, startY));
   const double fracStart = startTopH > 1e-9 ? std::min(1.0, std::max(0.0, S.z / startTopH)) : 0;
   const double fracEnd = next.Next();

   const double dx = endX - startX, dy = endY - startY;
   const double straightLen = hypot(dx, dy);
   const double turnMag = std::max(0.0, p.mtWobbleTurn);
   const double pathBudget = turnMag > 0 ? std::max(straightLen, straightLen * std::max(1.0, p.mtWobbleFactor)) : straightLen;
   const int steps = (int)std::min((double)MT_MAX_STEPS_PER_MT, std::max(4.0, JsRound(pathBudget / std::max(0.02, p.mtStepLen))));
   const double stepLen = pathBudget / steps;

   const double smoothLp = std::max(stepLen, p.mtSmoothLen);
   const double headingKickScale = sqrt(stepLen / smoothLp);
   const double stepTurnCap = p.mtMinTurnRadius > 0 ? 2 * asin(std::min(1.0, stepLen / (2 * p.mtMinTurnRadius))) : PI;
   double hx = 1, hy = 0;
   if (straightLen > 1e-9) { hx = dx / straightLen; hy = dy / straightLen; }
   std::vector<Pt2m> raw((size_t)steps + 1);
   raw[0] = { 0, 0 };
   for (int i = 1; i <= steps; i++) {
      if (turnMag > 0) {
         const double n1 = next.Next() * 2 - 1;
         double turnAngle = atan2(turnMag * headingKickScale * n1, 1);
         if (turnAngle > stepTurnCap) turnAngle = stepTurnCap;
         else if (turnAngle < -stepTurnCap) turnAngle = -stepTurnCap;
         const double cosT = cos(turnAngle), sinT = sin(turnAngle);
         const double nhx = hx * cosT - hy * sinT, nhy = hx * sinT + hy * cosT;
         hx = nhx; hy = nhy;
      }
      raw[i] = { raw[i - 1].x + hx * stepLen, raw[i - 1].y + hy * stepLen };
   }

   const double driftX = dx - raw[steps].x, driftY = dy - raw[steps].y;
   std::vector<Pt2m> ptsXY((size_t)steps + 1);
   for (int i = 0; i <= steps; i++) {
      const double t = (double)i / steps;
      const double s = t * t * (3 - 2 * t);
      ptsXY[i] = { startX + raw[i].x + driftX * s, startY + raw[i].y + driftY * s };
   }
   MtEnforceMinTurnRadius(ptsXY, p.mtMinTurnRadius);

   const double fracNoiseAmp = 0.15 * turnMag;
   const double fracNoiseDecay = exp(-stepLen / smoothLp);
   const double fracNoiseInnovScale = sqrt(std::max(0.0, 1 - fracNoiseDecay * fracNoiseDecay));
   double fracNoise = 0;

   // z without the nucleus: the fraction profile, then box-smoothed (endpoints kept).
   Microtubule mt;
   std::vector<Pt3>& pts = mt.pts;
   pts.resize((size_t)steps + 1);
   std::vector<double> ceil((size_t)steps + 1);
   for (int i = 0; i <= steps; i++) {
      const double t = (double)i / steps;
      const double s = t * t * (3 - 2 * t);
      const double x = ptsXY[i].x, y = ptsXY[i].y;
      ceil[i] = std::max(0.0, SampleCytoMeshHeight(c, g.mesh, x, y));
      double frac;
      if (i == 0) frac = fracStart;
      else if (i == steps) frac = fracEnd;
      else {
         const double u = next.Next();
         fracNoise = fracNoiseDecay * fracNoise + fracNoiseAmp * fracNoiseInnovScale * (u * 2 - 1);
         frac = std::min(1.0, std::max(0.0, lerp(fracStart, fracEnd, s) + fracNoise * sin(PI * t)));
      }
      pts[i] = { x, y, frac * ceil[i] };
   }
   const double smoothWinPts = std::max(0.0, JsRound(p.mtSmoothLen / stepLen));
   if (smoothWinPts > 0) {
      std::vector<double> z(pts.size());
      for (size_t i = 0; i < pts.size(); i++) z[i] = pts[i].z;
      MtBoxSmooth(z, (long)std::max(1.0, JsRound(smoothWinPts / 2)), [&](size_t i, double v) { pts[i].z = v; });
   }

   // Over or under the obstacles (the nucleus), under the ceiling, by a smooth ramp.
   {
      const MtNucleusObstacle nucleus(c, p);
      const std::vector<const MtObstacle*> obstacles{ &nucleus };
      std::vector<double> lo, hi;
      MtObstacleBounds(pts, ceil, obstacles, lo, hi);
      MtObstacleEnvelope(pts, lo, hi, (long)std::max(1.0, JsRound(0.5 * p.mtSmoothLen / stepLen)));
   }

   // Truncate at the first exit through the cell outline.
   size_t cutLen = pts.size();
   const double rIn = CellInnerRadiusBound(c);
   for (size_t i = 0; i < pts.size(); i++) {
      const double d = hypot(pts[i].x, pts[i].y);
      if (d <= rIn) continue;
      if (d > CellRadiusAt(c, atan2(pts[i].y, pts[i].x))) { cutLen = i; break; }
   }
   pts.resize(cutLen);
   for (Pt3& pt : pts) MtClampIntoCytoplasm(c, g, pt, true);

   mt.priority = next.Next();
   return mt;
}

std::vector<Microtubule> BuildMicrotubulesForCell(uint32_t seed, const Cell& c, const Params& p, const MtCellGeom& g)
{
   std::vector<Microtubule> mts;
   if (!c.present) return mts;
   const int count = std::min(MT_MAX_PER_CELL, MtCountForCell(seed, c, p, g));
   if (count <= 0) return mts;
   // Every microtubule draws from its own stream (seed, cell, index) and
   // reads only the cell, the parameters and the geometry: generated in
   // parallel into its slot, consumed in index order below.
   mts.resize((size_t)count);
   ParallelFor((size_t)count, 4, [&](size_t i) { mts[i] = MtGenerateOne(seed, (int)i, 0, c, p, g); });
   MtResolveCollisions(seed, c, p, mts, g);
   ParallelFor(mts.size(), 4, [&](size_t i) {
      Microtubule& m = mts[i];
      MtTrimSteepEnds(m.pts, p.mtMaxZSlope);
      MtLimitZSlopeRealized(m.pts, p.mtMaxZSlope);
      // Inside the outline from an earlier clamp (the cut, or the clamp after
      // a collision nudge); the two steps above change z only.
      for (Pt3& pt : m.pts) MtClampIntoCytoplasm(c, g, pt, true);
   });
   return mts;
}

MtFrames BuildMtFrames(const std::vector<Pt3>& pts)
{
   MtFrames f;
   const size_t n = pts.size();
   if (n < 2) { f.cum.assign(n, 0.0); return f; }
   f.cum.reserve(n); f.T.reserve(n - 1); f.U.reserve(n - 1); f.V.reserve(n - 1);
   f.cum.push_back(0);
   Pt3 prevU{ 0, 0, 0 };
   for (size_t i = 0; i + 1 < n; i++) {
      const double dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y, dz = pts[i + 1].z - pts[i].z;
      const double len = jsm::hypot(dx, dy, dz);
      f.cum.push_back(f.cum[i] + len);
      Pt3 t;
      if (len > 1e-12) t = { dx / len, dy / len, dz / len };
      else t = i > 0 ? f.T[i - 1] : Pt3{ 1, 0, 0 };
      Pt3 u = i == 0 ? (std::fabs(t.z) < 0.9 ? Pt3{ 0, 0, 1 } : Pt3{ 1, 0, 0 }) : prevU;
      const double d = u.x * t.x + u.y * t.y + u.z * t.z;
      u = { u.x - d * t.x, u.y - d * t.y, u.z - d * t.z };
      const double ul = NormOr1(jsm::hypot(u.x, u.y, u.z));
      u = { u.x / ul, u.y / ul, u.z / ul };
      f.T.push_back(t); f.U.push_back(u);
      f.V.push_back({ t.y * u.z - t.z * u.y, t.z * u.x - t.x * u.z, t.x * u.y - t.y * u.x });
      prevU = u;
   }
   return f;
}

} // namespace isc
