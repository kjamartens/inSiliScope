#include "microtubules.h"

#include "jsmath.h"
#include "rng.h"

#include <algorithm>
#include <cmath>
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

double MtNucleusClearance(const Params& p) { return std::max(0.05, 0.5 * p.nucMargin); }

double MtNucleusFootprintBlend(const Cell& c, double x, double y)
{
   const double dxN = x - c.nucOffX, dyN = y - c.nucOffY;
   const double cr = jsm::cos(-c.nucRot), sr = jsm::sin(-c.nucRot);
   const double lx = dxN * cr - dyN * sr, ly = dxN * sr + dyN * cr;
   const double a = c.nucLong / 2, b = c.nucShort / 2;
   const double norm = jsm::hypot(lx / std::max(1e-6, a), ly / std::max(1e-6, b));
   const double t = std::min(1.0, std::max(0.0, (norm - 1) / MT_NUCLEUS_CLEAR_BLEND));
   return Smoothstep(1 - t);
}

struct Pt2m { double x, y; };

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

double MtNucleusRadiusAt(const Cell& c, double theta)
{
   const double a = c.nucLong / 2, b = c.nucShort / 2;
   const double phi = theta - c.nucRot;
   return (a * b) / jsm::hypot(b * jsm::cos(phi), a * jsm::sin(phi));
}

double MtRayCellBoundaryFromNucleus(const Cell& c, double theta, double rNucStart)
{
   const double ux = jsm::cos(theta), uy = jsm::sin(theta);
   const double maxT = (c.rOuter + jsm::hypot(c.nucOffX, c.nucOffY)) * 1.3 + 1;
   const double dt = std::max(1e-3, (maxT - rNucStart) / MT_MARCH_STEPS);
   double prevT = rNucStart, tLo = 0, tHi = 0;
   bool found = false;
   for (int s = 0; s <= MT_MARCH_STEPS; s++) {
      const double t = rNucStart + s * dt;
      const double px = c.nucOffX + ux * t, py = c.nucOffY + uy * t;
      const double ang = jsm::atan2(py, px);
      const double rc = CellRadiusAt(c, ang);
      const double dist = jsm::hypot(px, py);
      if (dist > rc) { tHi = t; tLo = s == 0 ? t : prevT; found = true; break; }
      prevT = t;
   }
   if (!found) return prevT;
   for (int it = 0; it < MT_BISECT_ITERS; it++) {
      const double mid = (tLo + tHi) / 2;
      const double px = c.nucOffX + ux * mid, py = c.nucOffY + uy * mid;
      const double ang = jsm::atan2(py, px);
      const double rc = CellRadiusAt(c, ang);
      const double dist = jsm::hypot(px, py);
      if (dist > rc) tHi = mid; else tLo = mid;
   }
   return (tLo + tHi) / 2;
}

double MtSampleDirection(const MtCellGeom& g, double u1, double u2)
{
   if (g.dirTotal <= 1e-9) return u1 * jsm::PI * 2;
   const double target = u1 * g.dirTotal;
   int lo = 0, hi = MT_N_DIR;
   while (lo < hi) {
      const int mid = (lo + hi) >> 1;
      if (g.dirCum[mid + 1] < target) lo = mid + 1; else hi = mid;
   }
   const int i = std::min(MT_N_DIR - 1, lo);
   const double binWidth = (jsm::PI * 2) / MT_N_DIR;
   return g.dirTheta[i] + (u2 - 0.5) * binWidth;
}

void MtClampIntoCytoplasm(const Cell& c, const MtCellGeom& g, Pt3& pt)
{
   const double dxN = pt.x - c.nucOffX, dyN = pt.y - c.nucOffY;
   const double cr = jsm::cos(-c.nucRot), sr = jsm::sin(-c.nucRot);
   const double lx = dxN * cr - dyN * sr, ly = dxN * sr + dyN * cr;
   const double a = c.nucLong / 2, b = c.nucShort / 2, rz = c.nucHeight / 2;
   const double nz = (pt.z - c.nucZ) / std::max(1e-6, rz);
   const double nx = lx / std::max(1e-6, a), ny = ly / std::max(1e-6, b);
   const double ellNorm = jsm::sqrt(nx * nx + ny * ny + nz * nz);
   if (ellNorm < 1) {
      const double scale = (ellNorm > 1e-9 ? 1 / ellNorm : 1) / MT_CONTAIN_MARGIN;
      const double lx2 = lx * scale, ly2 = ly * scale;
      pt.z = c.nucZ + nz * rz * scale;
      const double cr2 = jsm::cos(c.nucRot), sr2 = jsm::sin(c.nucRot);
      pt.x = c.nucOffX + lx2 * cr2 - ly2 * sr2;
      pt.y = c.nucOffY + lx2 * sr2 + ly2 * cr2;
   }
   const double ang = jsm::atan2(pt.y, pt.x);
   const double rc = CellRadiusAt(c, ang);
   const double dist = jsm::hypot(pt.x, pt.y);
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
   g.dirCum[0] = 0;
   for (int i = 0; i < MT_N_DIR; i++) {
      const double theta = ((double)i / MT_N_DIR) * jsm::PI * 2;
      const double rNuc = MtNucleusRadiusAt(c, theta);
      const double rCell = MtRayCellBoundaryFromNucleus(c, theta, rNuc);
      const double w = std::max(0.0, rCell - rNuc);
      g.dirTheta[i] = theta;
      g.dirCum[i + 1] = g.dirCum[i] + w;
   }
   g.dirTotal = g.dirCum[MT_N_DIR];
   g.areaUm2 = MtShoelaceArea(CellOutlineLocal(c, 48));
   g.mesh = BuildCytoMesh(c, p);
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

   const double u1 = next.Next();
   const double u2 = next.Next();
   const double theta0 = MtSampleDirection(g, u1, u2);
   const double phi0 = theta0 - c.nucRot;
   const double cosPsi = 1 - 2 * next.Next();
   const double sinPsi = sqrt(std::max(0.0, 1 - cosPsi * cosPsi));
   const double aN = c.nucLong / 2, bN = c.nucShort / 2, rzN = c.nucHeight / 2;
   const double lx0 = aN * sinPsi * cos(phi0), ly0 = bN * sinPsi * sin(phi0), lz0 = rzN * cosPsi;
   const double crN = cos(c.nucRot), srN = sin(c.nucRot);
   const double surfX = c.nucOffX + lx0 * crN - ly0 * srN;
   const double surfY = c.nucOffY + lx0 * srN + ly0 * crN;
   const double surfZ = c.nucZ + lz0;
   double ox = surfX - c.nucOffX, oy = surfY - c.nucOffY, oz = lz0;
   {
      const double n = NormOr1(hypot(ox, oy, oz));
      ox = ox / n; oy = oy / n; oz = oz / n;
   }

   const double rNuc0 = MtNucleusRadiusAt(c, theta0);
   const double rCell0 = MtRayCellBoundaryFromNucleus(c, theta0, rNuc0);
   const double startFrac = lerp(p.mtStartFracMin, p.mtStartFracMax, next.Next());
   const double startDist = startFrac * rCell0;
   double startX = surfX + ox * startDist;
   double startY = surfY + oy * startDist;
   const double startZ = surfZ + oz * startDist;

   const double offR = next.Next() * std::max(0.0, p.mtStartOffsetXY);
   const double offAng = next.Next() * PI * 2;
   startX += cos(offAng) * offR;
   startY += sin(offAng) * offR;

   const double jitterRad = (next.Next() * 2 - 1) * p.mtEndJitterDeg * PI / 180;
   const double thetaEnd = theta0 + jitterRad;
   const double rNucEnd = MtNucleusRadiusAt(c, thetaEnd);
   const double rCell1 = MtRayCellBoundaryFromNucleus(c, thetaEnd, rNucEnd);
   const double endFrac = lerp(p.mtEndFracMin, p.mtEndFracMax, next.Next());
   const double endR = std::max(0.0, rCell1 - endFrac * rCell1);
   const double endX = c.nucOffX + cos(thetaEnd) * endR;
   const double endY = c.nucOffY + sin(thetaEnd) * endR;

   const double startTopH = std::max(0.0, SampleCytoMeshHeight(c, g.mesh, startX, startY));
   const double fracStart = startTopH > 1e-9 ? std::min(1.0, std::max(0.0, startZ / startTopH)) : 0;
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

   const double nucTopZ = c.nucZ + c.nucHeight / 2;
   const double nucBottomZ = c.nucZ - c.nucHeight / 2;
   const double nucClearance = MtNucleusClearance(p);
   const bool canGoUnder = nucBottomZ - nucClearance > 0;
   const bool goOverNucleus = !canGoUnder || cosPsi >= 0;
   auto applyNucleusOverride = [&](double x, double y, double zNormal, double ceilH) {
      const double blend = MtNucleusFootprintBlend(c, x, y);
      if (blend <= 0) return zNormal;
      const double target = goOverNucleus
         ? std::max(zNormal, std::min(ceilH, nucTopZ + nucClearance))
         : std::min(zNormal, std::max(0.0, nucBottomZ - nucClearance));
      return lerp(zNormal, target, blend);
   };

   Microtubule mt;
   std::vector<Pt3>& pts = mt.pts;
   pts.resize((size_t)steps + 1);
   for (int i = 0; i <= steps; i++) {
      const double t = (double)i / steps;
      const double s = t * t * (3 - 2 * t);
      const double x = ptsXY[i].x, y = ptsXY[i].y;
      const double ceilH = std::max(0.0, SampleCytoMeshHeight(c, g.mesh, x, y));
      double z;
      if (i == 0) {
         z = fracStart * ceilH;
      } else if (i == steps) {
         z = applyNucleusOverride(x, y, fracEnd * ceilH, ceilH);
      } else {
         const double u = next.Next();
         fracNoise = fracNoiseDecay * fracNoise + fracNoiseAmp * fracNoiseInnovScale * (u * 2 - 1);
         const double frac = std::min(1.0, std::max(0.0, lerp(fracStart, fracEnd, s) + fracNoise * sin(PI * t)));
         z = applyNucleusOverride(x, y, frac * ceilH, ceilH);
      }
      pts[i] = { x, y, z };
   }

   const double smoothWinPts = std::max(0.0, JsRound(p.mtSmoothLen / stepLen));
   if (smoothWinPts > 0) {
      std::vector<double> zOrig(pts.size());
      for (size_t i = 0; i < pts.size(); i++) zOrig[i] = pts[i].z;
      const long half = (long)std::max(1.0, JsRound(smoothWinPts / 2));
      const long last = (long)pts.size() - 1;
      for (long i = 1; i < last; i++) {
         const long lo = std::max(0L, i - half), hi = std::min(last, i + half);
         double sum = 0; int n = 0;
         for (long j = lo; j <= hi; j++) { sum += zOrig[j]; n++; }
         pts[i].z = sum / n;
      }
   }

   const double maxDz = std::max(1.0, p.mtMaxZSlope) * stepLen;
   for (int pass = 0; pass < 20; pass++) {
      bool changed = false;
      const bool forward = pass % 2 == 0;
      const size_t n = pts.size();
      for (size_t k = 1; k + 1 < n; k++) {
         const size_t i = forward ? k : n - 1 - k;
         const Pt3& prev = pts[i - 1]; Pt3& cur = pts[i]; const Pt3& nxt = pts[i + 1];
         const double lo = std::max(prev.z - maxDz, nxt.z - maxDz);
         const double hi = std::min(prev.z + maxDz, nxt.z + maxDz);
         if (lo > hi) continue;
         if (cur.z < lo) { cur.z = lo; changed = true; }
         else if (cur.z > hi) { cur.z = hi; changed = true; }
      }
      if (!changed) break;
   }

   // Truncate at the first exit through the cell outline.
   size_t cutLen = pts.size();
   for (size_t i = 0; i < pts.size(); i++) {
      const double ang = atan2(pts[i].y, pts[i].x);
      if (hypot(pts[i].x, pts[i].y) > CellRadiusAt(c, ang)) { cutLen = i; break; }
   }
   pts.resize(cutLen);
   for (Pt3& pt : pts) MtClampIntoCytoplasm(c, g, pt);

   mt.priority = next.Next();
   return mt;
}

std::vector<Microtubule> BuildMicrotubulesForCell(uint32_t seed, const Cell& c, const Params& p, const MtCellGeom& g)
{
   std::vector<Microtubule> mts;
   if (!c.present) return mts;
   const int count = std::min(MT_MAX_PER_CELL, MtCountForCell(seed, c, p, g));
   if (count <= 0) return mts;
   mts.reserve((size_t)count);
   for (int i = 0; i < count; i++) mts.push_back(MtGenerateOne(seed, i, 0, c, p, g));
   MtResolveCollisions(seed, c, p, mts, g);
   for (Microtubule& m : mts) {
      MtTrimSteepEnds(m.pts, p.mtMaxZSlope);
      MtLimitZSlopeRealized(m.pts, p.mtMaxZSlope);
      for (Pt3& pt : m.pts) MtClampIntoCytoplasm(c, g, pt);
   }
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
