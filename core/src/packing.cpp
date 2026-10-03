#include "packing.h"

#include "jsmath.h"
#include "parallel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace isc {

namespace {

constexpr double TWO_PI = jsm::PI * 2;
constexpr double PACK_ROT_TRIAL = 12 * jsm::PI / 180;

double RadiusFromLUT(const float* lut, double thetaLocal)
{
   // fmod(x, 2 pi) is x itself (exactly) when |x| < 2 pi, the usual case:
   // the call is skipped then (NaN takes the fmod path and stays NaN).
   double t = thetaLocal;
   if (!(t > -TWO_PI && t < TWO_PI)) t = std::fmod(thetaLocal, TWO_PI);
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

// A cell's collision outline in the world frame at rotation `rot` (the 16
// collisionLocal points turned and moved) with that rotation's cos/sin.
// Within a Jacobi iteration every cell's pose is fixed, so it is computed
// once per cell (and once per rotation trial) instead of once per neighbour
// pair: the same expressions on the same operands as before.
struct CellPose {
   double cosR, sinR;
   double w[OUTLINE_SAMPLE_N * 2];
};

void MakePose(const Cell& c, double rot, CellPose& o)
{
   o.cosR = jsm::cos(rot);
   o.sinR = jsm::sin(rot);
   for (int i = 0; i < OUTLINE_SAMPLE_N; i++) {
      const double lx = c.collisionLocal[i * 2], ly = c.collisionLocal[i * 2 + 1];
      o.w[i * 2] = c.x + lx * o.cosR - ly * o.sinR;
      o.w[i * 2 + 1] = c.y + lx * o.sinR + ly * o.cosR;
   }
}

// cp: c's pose at crot; np: n's pose at nrot (= n.packRot).
double BoundaryClearance(const Cell& c, const CellPose& cp, double crot, const Cell& n, const CellPose& np, double nrot)
{
   using namespace jsm;
   double minGap = std::numeric_limits<double>::infinity();
   for (int i = 0; i < OUTLINE_SAMPLE_N; i++) {
      const double bdx = cp.w[i * 2] - n.x, bdy = cp.w[i * 2 + 1] - n.y;
      const double bd = hypot(bdx, bdy);
      const double gap = bd - RadiusFromLUT(n.radiusLUT, atan2(bdy, bdx) - nrot);
      if (gap < minGap) minGap = gap;
   }
   for (int i = 0; i < OUTLINE_SAMPLE_N; i++) {
      const double adx = np.w[i * 2] - c.x, ady = np.w[i * 2 + 1] - c.y;
      const double ad = hypot(adx, ady);
      const double gap = ad - RadiusFromLUT(c.radiusLUT, atan2(ady, adx) - crot);
      if (gap < minGap) minGap = gap;
   }
   return minGap;
}

struct Overlap { double overlap, ux, uy; };

// Overlap measured against the "min gap x combined radius" meaning:
// minGapFrac < 1 allows (ra+rb)*(1-minGapFrac) of penetration.
Overlap DirectionalOverlap(const Cell& c, const CellPose& cp, double crot, const Cell& n, const CellPose& np,
                           double minGapFrac)
{
   using namespace jsm;
   const double dx = n.x - c.x, dy = n.y - c.y;
   double d = hypot(dx, dy);
   if (d == 0 || std::isnan(d)) d = 1e-6;   // JS: hypot(...) || 1e-6
   const double thetaAB = atan2(dy, dx);
   const double ra = RadiusFromLUT(c.radiusLUT, thetaAB - crot);
   const double rb = RadiusFromLUT(n.radiusLUT, thetaAB + PI - n.packRot);
   const double allowedPenetration = (ra + rb) * (1 - minGapFrac);
   const double clearance = BoundaryClearance(c, cp, crot, n, np, n.packRot);
   return { -allowedPenetration - clearance, dx / d, dy / d };
}

// c at the trial rotation against its overlapping neighbours (indices into
// map.cells / poses).
double TotalDirOverlap(const Cell& c, const std::vector<Cell>& cells, const std::vector<CellPose>& poses,
                       const std::vector<int32_t>& neighbors, double minGapFrac, double packRotTrial)
{
   CellPose cp;
   MakePose(c, packRotTrial, cp);
   double total = 0;
   for (int32_t j : neighbors) {
      const double ov = DirectionalOverlap(c, cp, packRotTrial, cells[j], poses[j], minGapFrac).overlap;
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
   const size_t W = (size_t)m.Width(), H = (size_t)m.Height();
   m.grid.assign(W * H, -1);
   // Every candidate is a pure function of (seed, chunk, params): made in
   // parallel, then taken in the JS insertion order (cx-major, then cy).
   std::vector<Cell> raw(W * H);
   ParallelFor(W * H, 8, [&](size_t idx) {
      raw[idx] = RawCandidate(seed, cx0 + (int32_t)(idx / H), cy0 + (int32_t)(idx % H), p);
   });
   m.cells.reserve(W * H);
   for (int32_t cx = cx0; cx <= cx1; cx++)
      for (int32_t cy = cy0; cy <= cy1; cy++) {
         const Cell& c = raw[(size_t)(cx - cx0) * H + (size_t)(cy - cy0)];
         if (!c.present) continue;
         m.grid[(size_t)(cx - cx0) * H + (size_t)(cy - cy0)] = (int32_t)m.cells.size();
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
   std::vector<CellPose> poses(N);
   for (int it = 0; it < iters; it++) {
      // The poses of this iteration's snapshot.
      for (size_t i = 0; i < N; i++)
         if (map.alive[i]) MakePose(map.cells[i], map.cells[i].packRot, poses[i]);
      // Jacobi: disp[i] reads only the snapshot (cells, poses) and writes its
      // own slot, so the cells run in parallel; the update below is serial.
      ParallelFor(N, 8, [&](size_t i) {
         if (!map.alive[i]) return;
         thread_local std::vector<int32_t> neighbors, overlapping;
         const Cell& c = map.cells[i];
         // Circle-sum broad phase before the expensive outline check.
         neighbors.clear();
         for (int32_t j : chunkNbrs[i]) {
            const Cell& n = map.cells[j];
            const double dxp = n.x - c.x, dyp = n.y - c.y;
            const double maxD = c.rOuter + n.rOuter;
            if (dxp * dxp + dyp * dyp <= maxD * maxD) neighbors.push_back(j);
         }

         double ax = 0, ay = 0, curOverlapSum = 0;
         overlapping.clear();
         for (int32_t j : neighbors) {
            const Overlap o = DirectionalOverlap(c, poses[i], c.packRot, map.cells[j], poses[j], minGapFrac);
            if (o.overlap > 0) {
               curOverlapSum += o.overlap;
               overlapping.push_back(j);
               ax += -o.ux * o.overlap * 0.5;
               ay += -o.uy * o.overlap * 0.5;
            }
         }

         double rotDelta = 0;
         if (allowRot && !overlapping.empty()) {
            const double plus = TotalDirOverlap(c, map.cells, poses, overlapping, minGapFrac, c.packRot + PACK_ROT_TRIAL);
            const double minus = TotalDirOverlap(c, map.cells, poses, overlapping, minGapFrac, c.packRot - PACK_ROT_TRIAL);
            if (plus < curOverlapSum && plus <= minus) rotDelta = PACK_ROT_TRIAL;
            else if (minus < curOverlapSum) rotDelta = -PACK_ROT_TRIAL;
         }
         disp[i] = { ax * damping, ay * damping, rotDelta * damping };
      });
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
   std::vector<CellPose> poses(map.cells.size());
   for (size_t i = 0; i < map.cells.size(); i++)
      if (map.alive[i]) MakePose(map.cells[i], map.cells[i].packRot, poses[i]);
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
            if (DirectionalOverlap(c, poses[i], c.packRot, n, poses[j], minGapFrac).overlap > 0)
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
