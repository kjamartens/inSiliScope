#include "world.h"

#include "jsmath.h"
#include "packing.h"

#include <algorithm>
#include <cmath>

namespace isc {

namespace {

constexpr size_t BLOCK_CACHE_MAX = 4096;
// Farthest a dye sits from its microtubule axis: radius + binder + max linker.
constexpr double DYE_REACH_UM = (MT_RADIUS_NM + MT_BINDER_NM + MT_LINKER_MAX_NM) * 1e-3;

inline int32_t FloorDiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Distance from (px, py) to the rect (0 inside).
inline double RectDist(double px, double py, double x0, double y0, double x1, double y1)
{
   const double dx = std::max(std::max(x0 - px, 0.0), px - x1);
   const double dy = std::max(std::max(y0 - py, 0.0), py - y1);
   return jsm::hypot(dx, dy);
}

Pt3 PointAtArc(const std::vector<Pt3>& pts, const MtFrames& fr, double S)
{
   const size_t seg = MtSegmentAt(fr, S);
   const double f = S - fr.cum[seg];
   const Pt3& t = fr.T[seg];
   return { pts[seg].x + t.x * f, pts[seg].y + t.y * f, pts[seg].z + t.z * f };
}

// Upper bound on how far a cell's footprint reaches from its home chunk:
// the worst-case rOuter (as InteractionChunks) twice over, plus a chunk for
// the packing displacement.
double CellReachUm(const Params& p)
{
   const double worstSemiMajor = (p.cellDiamMax / 2) / jsm::sqrt(std::max(0.05, p.cellElongMin));
   return 2 * worstSemiMajor * CELL_MOD_MAX + p.chunkSize;
}

} // namespace

void LocalToWorld(const Cell& c, double lx, double ly, double& wx, double& wy)
{
   const double rot = c.packRot;
   if (rot == 0 || std::isnan(rot)) { wx = c.x + lx; wy = c.y + ly; return; }
   const double cr = jsm::cos(rot), sr = jsm::sin(rot);
   wx = c.x + lx * cr - ly * sr;
   wy = c.y + lx * sr + ly * cr;
}

const MtFrames& CellAssets::Frames(size_t i)
{
   if (!frames[i]) frames[i].reset(new MtFrames(BuildMtFrames(mts[i].pts)));
   return *frames[i];
}

World::World(uint32_t seed, const Params& p, size_t assetCacheCells)
   : seed_(seed), p_(p), assetCap_(std::max<size_t>(1, assetCacheCells))
{
   NormalizeParams(p_);
}

void World::DropCaches()
{
   blocks_.clear();
   assets_.clear();
}

const std::vector<Cell>& World::PackedBlock(int32_t bx, int32_t by)
{
   const auto key = std::make_pair(bx, by);
   auto it = blocks_.find(key);
   if (it != blocks_.end()) return it->second;
   if (blocks_.size() >= BLOCK_CACHE_MAX) blocks_.clear();

   const int32_t cx0 = bx * PACK_BLOCK_CHUNKS, cy0 = by * PACK_BLOCK_CHUNKS;
   const int32_t cx1 = cx0 + PACK_BLOCK_CHUNKS - 1, cy1 = cy0 + PACK_BLOCK_CHUNKS - 1;
   std::vector<Cell> cells;
   if (p_.enablePacking) {
      const int32_t M = InteractionChunks(p_) + 2;
      CandidateMap m = BuildCandidateMap(seed_, cx0 - M, cy0 - M, cx1 + M, cy1 + M, p_);
      PackMap(m, p_);
      for (size_t i = 0; i < m.cells.size(); i++) {
         const Cell& c = m.cells[i];
         if (m.alive[i] && c.cx >= cx0 && c.cx <= cx1 && c.cy >= cy0 && c.cy <= cy1) cells.push_back(c);
      }
   } else {
      for (int32_t cx = cx0; cx <= cx1; cx++)
         for (int32_t cy = cy0; cy <= cy1; cy++) {
            Cell c = RawCandidate(seed_, cx, cy, p_);
            if (c.present) cells.push_back(c);
         }
   }
   stats_.blocksPacked++;
   return blocks_.emplace(key, std::move(cells)).first->second;
}

void World::CellsInRect(double x0, double y0, double x1, double y1, std::vector<Cell>& out)
{
   const double S = p_.chunkSize, reach = CellReachUm(p_);
   const int32_t cxLo = (int32_t)std::floor((x0 - reach) / S), cxHi = (int32_t)std::floor((x1 + reach) / S);
   const int32_t cyLo = (int32_t)std::floor((y0 - reach) / S), cyHi = (int32_t)std::floor((y1 + reach) / S);
   const int32_t bx0 = FloorDiv(cxLo, PACK_BLOCK_CHUNKS), bx1 = FloorDiv(cxHi, PACK_BLOCK_CHUNKS);
   const int32_t by0 = FloorDiv(cyLo, PACK_BLOCK_CHUNKS), by1 = FloorDiv(cyHi, PACK_BLOCK_CHUNKS);
   for (int32_t bx = bx0; bx <= bx1; bx++)
      for (int32_t by = by0; by <= by1; by++)
         for (const Cell& c : PackedBlock(bx, by))
            if (RectDist(c.x, c.y, x0, y0, x1, y1) <= c.rOuter) out.push_back(c);
}

CellAssets& World::Assets(const Cell& c)
{
   const auto key = std::make_pair(c.cx, c.cy);
   for (auto it = assets_.begin(); it != assets_.end(); ++it) {
      if (it->first != key) continue;
      if (it != assets_.begin()) assets_.splice(assets_.begin(), assets_, it);
      return *assets_.front().second;
   }
   std::unique_ptr<CellAssets> a(new CellAssets());
   a->cell = c;
   a->geom = BuildMtCellGeom(c, p_);
   a->mts = BuildMicrotubulesForCell(seed_, c, p_, a->geom);
   a->mtReach.resize(a->mts.size());
   a->frames.resize(a->mts.size());
   for (size_t i = 0; i < a->mts.size(); i++) {
      double r = 0;
      for (const Pt3& q : a->mts[i].pts) r = std::max(r, jsm::hypot(q.x, q.y));
      a->mtReach[i] = r;
   }
   stats_.cellsBuilt++;
   assets_.emplace_front(key, std::move(a));
   while (assets_.size() > assetCap_) assets_.pop_back();
   return *assets_.front().second;
}

void World::SitesInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                          std::vector<WorldDye>& out)
{
   std::vector<Cell> cells;
   CellsInRect(x0, y0, x1, y1, cells);
   const double eff = p_.labelEfficiency;
   const double blockReach = DYE_BLOCK_UM / 2 + DYE_REACH_UM;
   std::vector<Dye> dyes;
   for (const Cell& c : cells) {
      CellAssets& A = Assets(c);
      const double centreDist = RectDist(c.x, c.y, x0, y0, x1, y1);
      for (size_t i = 0; i < A.mts.size(); i++) {
         const std::vector<Pt3>& pts = A.mts[i].pts;
         if (pts.size() < 2 || centreDist > A.mtReach[i] + DYE_REACH_UM) continue;
         const MtFrames& fr = A.Frames(i);
         const double len = fr.Length();
         const int nBlocks = (int)std::ceil(len / DYE_BLOCK_UM);
         for (int b = 0; b < nBlocks; b++) {
            // Every point of block b lies within half a block (arc) of its midpoint.
            const Pt3 mid = PointAtArc(pts, fr, std::min((b + 0.5) * DYE_BLOCK_UM, len));
            if (mid.z + blockReach < zMin || mid.z - blockReach >= zMax) continue;
            double wx, wy;
            LocalToWorld(c, mid.x, mid.y, wx, wy);
            if (RectDist(wx, wy, x0, y0, x1, y1) > blockReach) continue;
            dyes.clear();
            DyesInBlock(seed_, c.cx, c.cy, (int)i, pts, fr, b, eff, dyes);
            stats_.dyeBlocks++;
            for (const Dye& d : dyes) {
               if (!(d.pos.z >= zMin && d.pos.z < zMax)) continue;
               double dx, dy;
               LocalToWorld(c, d.pos.x, d.pos.y, dx, dy);
               if (!(dx >= x0 && dx < x1 && dy >= y0 && dy < y1)) continue;
               out.push_back({ dx, dy, d.pos.z, d.id, c.cx, c.cy, d.mtIndex });
            }
         }
      }
   }
}

long World::DensityInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                            int nx, int ny, float* out)
{
   std::fill(out, out + (size_t)nx * ny, 0.0f);
   std::vector<WorldDye> dyes;
   SitesInWindow(x0, y0, x1, y1, zMin, zMax, dyes);
   const double sx = nx / (x1 - x0), sy = ny / (y1 - y0);
   for (const WorldDye& d : dyes) {
      const int ix = std::min(nx - 1, (int)std::floor((d.x - x0) * sx));
      const int iy = std::min(ny - 1, (int)std::floor((d.y - y0) * sy));
      out[(size_t)iy * nx + ix] += 1;
   }
   return (long)dyes.size();
}

} // namespace isc
