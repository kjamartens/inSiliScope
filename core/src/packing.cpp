#include "packing.h"

#include "jsmath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace isc {

namespace {

constexpr double TWO_PI = jsm::PI * 2;
constexpr double PACK_ROT_TRIAL = 12 * jsm::PI / 180;

double RadiusFromLUT(const float* lut, double thetaLocal)
{
   double t = std::fmod(thetaLocal, TWO_PI);
   if (t < 0) t += TWO_PI;
   const double f = (t / TWO_PI) * RADIUS_LUT_N;
   const int i0 = (int)f, i1 = (i0 + 1) % RADIUS_LUT_N;
   const double frac = f - i0;
   // JS reads lut[128] (undefined -> NaN) when a tiny negative t rounds up to
   // exactly TWO_PI. Reproduced so both sides take the same decisions.
   if (i0 >= RADIUS_LUT_N) return std::numeric_limits<double>::quiet_NaN();
   return lut[i0] * (1 - frac) + lut[i1] * frac;
}

// r(theta) is a pure function of the local shape, so it is tabulated once
// per candidate (float, as the JS Float32Array) together with the 16 fixed
// collision-outline points in the packRot=0 frame.
void EnsureLUT(Cell& c)
{
   if (c.lutReady) return;
   for (int i = 0; i < RADIUS_LUT_N; i++)
      c.radiusLUT[i] = (float)CellRadiusAt(c, ((double)i / RADIUS_LUT_N) * jsm::PI * 2);
   for (int i = 0; i < OUTLINE_SAMPLE_N; i++) {
      const double th = ((double)i / OUTLINE_SAMPLE_N) * TWO_PI;
      const double r = RadiusFromLUT(c.radiusLUT, th);
      c.collisionLocal[i * 2] = (float)(r * jsm::cos(th));
      c.collisionLocal[i * 2 + 1] = (float)(r * jsm::sin(th));
   }
   c.lutReady = true;
}

double BoundaryClearance(const Cell& c, double crot, const Cell& n, double nrot)
{
   using namespace jsm;
   double minGap = std::numeric_limits<double>::infinity();
   const double cCos = cos(crot), cSin = sin(crot);
   const double nCos = cos(nrot), nSin = sin(nrot);
   for (int i = 0; i < OUTLINE_SAMPLE_N; i++) {
      const double lx = c.collisionLocal[i * 2], ly = c.collisionLocal[i * 2 + 1];
      const double wx = c.x + lx * cCos - ly * cSin, wy = c.y + lx * cSin + ly * cCos;
      const double bdx = wx - n.x, bdy = wy - n.y;
      const double bd = hypot(bdx, bdy);
      const double gap = bd - RadiusFromLUT(n.radiusLUT, atan2(bdy, bdx) - nrot);
      if (gap < minGap) minGap = gap;
   }
   for (int i = 0; i < OUTLINE_SAMPLE_N; i++) {
      const double lx = n.collisionLocal[i * 2], ly = n.collisionLocal[i * 2 + 1];
      const double wx = n.x + lx * nCos - ly * nSin, wy = n.y + lx * nSin + ly * nCos;
      const double adx = wx - c.x, ady = wy - c.y;
      const double ad = hypot(adx, ady);
      const double gap = ad - RadiusFromLUT(c.radiusLUT, atan2(ady, adx) - crot);
      if (gap < minGap) minGap = gap;
   }
   return minGap;
}

struct Overlap { double overlap, ux, uy; };

// Overlap measured against the "min gap x combined radius" meaning:
// minGapFrac < 1 allows (ra+rb)*(1-minGapFrac) of penetration.
Overlap DirectionalOverlap(const Cell& c, double crot, const Cell& n, double minGapFrac)
{
   using namespace jsm;
   const double dx = n.x - c.x, dy = n.y - c.y;
   double d = hypot(dx, dy);
   if (d == 0 || std::isnan(d)) d = 1e-6;   // JS: hypot(...) || 1e-6
   const double thetaAB = atan2(dy, dx);
   const double ra = RadiusFromLUT(c.radiusLUT, thetaAB - crot);
   const double rb = RadiusFromLUT(n.radiusLUT, thetaAB + PI - n.packRot);
   const double allowedPenetration = (ra + rb) * (1 - minGapFrac);
   const double clearance = BoundaryClearance(c, crot, n, n.packRot);
   return { -allowedPenetration - clearance, dx / d, dy / d };
}

double TotalDirOverlap(const Cell& c, const std::vector<const Cell*>& neighbors, double minGapFrac, double packRotTrial)
{
   double total = 0;
   for (const Cell* n : neighbors) {
      const double ov = DirectionalOverlap(c, packRotTrial, *n, minGapFrac).overlap;
      if (ov > 0) total += ov;
   }
   return total;
}

int LoopCount(double iters) { return iters > 0 ? (int)std::ceil(iters) : 0; }

} // namespace

int32_t CandidateMap::Find(int32_t cx, int32_t cy) const
{
   if (cx < cx0 || cx > cx1 || cy < cy0 || cy > cy1) return -1;
   const int32_t i = grid[(size_t)(cx - cx0) * Height() + (cy - cy0)];
   return (i >= 0 && alive[i]) ? i : -1;
}

int InteractionChunks(const Params& p)
{
   const double worstSemiMajor = (p.cellDiamMax / 2) / jsm::sqrt(std::max(0.05, p.cellElongMin));
   const double worstROuter = worstSemiMajor * CELL_MOD_MAX * (p.cellRough > 0 && p.cellBlob > 0 ? 1 + TAIL_MAX : 1);
   return (int)std::min(6.0, std::max(1.0, std::ceil((2 * worstROuter) / p.chunkSize)));
}

CandidateMap BuildCandidateMap(uint32_t seed, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1, const Params& p)
{
   CandidateMap m;
   m.cx0 = cx0; m.cy0 = cy0; m.cx1 = cx1; m.cy1 = cy1;
   m.grid.assign((size_t)m.Width() * m.Height(), -1);
   for (int32_t cx = cx0; cx <= cx1; cx++)
      for (int32_t cy = cy0; cy <= cy1; cy++) {
         Cell c = RawCandidate(seed, cx, cy, p);
         if (!c.present) continue;
         m.grid[(size_t)(cx - cx0) * m.Height() + (cy - cy0)] = (int32_t)m.cells.size();
         m.cells.push_back(c);
      }
   m.alive.assign(m.cells.size(), 1);
   return m;
}

void Relax(CandidateMap& map, const Params& p, int iters)
{
   const int NR = InteractionChunks(p);
   const double damping = p.relaxDamping;
   const double minGapFrac = p.packFrac;
   const bool allowRot = p.allowPackRotation;
   const size_t N = map.cells.size();

   for (Cell& c : map.cells) EnsureLUT(c);

   // Chunk adjacency never changes during relaxation (only x/y/packRot do).
   std::vector<std::vector<int32_t>> chunkNbrs(N);
   for (size_t i = 0; i < N; i++) {
      if (!map.alive[i]) continue;
      const Cell& c = map.cells[i];
      for (int dx = -NR; dx <= NR; dx++)
         for (int dy = -NR; dy <= NR; dy++) {
            if (dx == 0 && dy == 0) continue;
            const int32_t j = map.Find(c.cx + dx, c.cy + dy);
            if (j >= 0) chunkNbrs[i].push_back(j);
         }
   }

   struct Disp { double ax, ay, ar; };
   std::vector<Disp> disp(N);
   std::vector<const Cell*> neighbors, overlapping;
   for (int it = 0; it < iters; it++) {
      for (size_t i = 0; i < N; i++) {
         if (!map.alive[i]) continue;
         const Cell& c = map.cells[i];
         // Circle-sum broad phase before the expensive outline check.
         neighbors.clear();
         for (int32_t j : chunkNbrs[i]) {
            const Cell& n = map.cells[j];
            const double dxp = n.x - c.x, dyp = n.y - c.y;
            const double maxD = c.rOuter + n.rOuter;
            if (dxp * dxp + dyp * dyp <= maxD * maxD) neighbors.push_back(&n);
         }

         double ax = 0, ay = 0, curOverlapSum = 0;
         overlapping.clear();
         for (const Cell* n : neighbors) {
            const Overlap o = DirectionalOverlap(c, c.packRot, *n, minGapFrac);
            if (o.overlap > 0) {
               curOverlapSum += o.overlap;
               overlapping.push_back(n);
               ax += -o.ux * o.overlap * 0.5;
               ay += -o.uy * o.overlap * 0.5;
            }
         }

         double rotDelta = 0;
         if (allowRot && !overlapping.empty()) {
            const double plus = TotalDirOverlap(c, overlapping, minGapFrac, c.packRot + PACK_ROT_TRIAL);
            const double minus = TotalDirOverlap(c, overlapping, minGapFrac, c.packRot - PACK_ROT_TRIAL);
            if (plus < curOverlapSum && plus <= minus) rotDelta = PACK_ROT_TRIAL;
            else if (minus < curOverlapSum) rotDelta = -PACK_ROT_TRIAL;
         }
         disp[i] = { ax * damping, ay * damping, rotDelta * damping };
      }
      for (size_t i = 0; i < N; i++) {
         if (!map.alive[i]) continue;
         Cell& c = map.cells[i];
         c.x += disp[i].ax; c.y += disp[i].ay;
         if (disp[i].ar != 0) c.packRot = c.packRot + disp[i].ar;
      }
   }
}

int Prune(CandidateMap& map, const Params& p)
{
   const int NR = InteractionChunks(p);
   const double minGapFrac = p.packFrac;
   for (Cell& c : map.cells) EnsureLUT(c);
   std::vector<uint8_t> toRemove(map.cells.size(), 0);
   for (size_t i = 0; i < map.cells.size(); i++) {
      if (!map.alive[i]) continue;
      const Cell& c = map.cells[i];
      for (int dx = -NR; dx <= NR; dx++)
         for (int dy = -NR; dy <= NR; dy++) {
            if (dx == 0 && dy == 0) continue;
            const int32_t j = map.Find(c.cx + dx, c.cy + dy);
            if (j < 0) continue;
            const Cell& n = map.cells[j];
            const double dxp = n.x - c.x, dyp = n.y - c.y;
            const double maxD = c.rOuter + n.rOuter;
            if (dxp * dxp + dyp * dyp > maxD * maxD) continue;
            if (DirectionalOverlap(c, c.packRot, n, minGapFrac).overlap > 0)
               toRemove[c.priority >= n.priority ? j : i] = 1;
         }
   }
   int removed = 0;
   for (size_t i = 0; i < toRemove.size(); i++)
      if (toRemove[i]) { map.alive[i] = 0; removed++; }
   return removed;
}

int PackMap(CandidateMap& map, const Params& p)
{
   int removed = 0;
   Relax(map, p, LoopCount(p.relaxIters));
   for (int round = 0; round < PRUNE_ROUNDS; round++) {
      const int r = Prune(map, p);
      removed += r;
      if (r == 0) break;
      Relax(map, p, LoopCount(std::min(8.0, p.relaxIters)));
   }
   return removed;
}

} // namespace isc
