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

World::World(uint32_t seed, const Params& p, size_t assetCacheCells, size_t dyeCacheDyes)
   : seed_(seed), p_(p), assetCap_(std::max<size_t>(1, assetCacheCells)), dyeCap_(dyeCacheDyes)
{
   NormalizeParams(p_);
}

void World::DropCaches()
{
   blocks_.clear();
   assets_.clear();
   dyeLru_.clear();
   dyeIndex_.clear();
   dyeCount_ = 0;
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

template <class Fn>
void World::ForEachDyeBlock(double x0, double y0, double x1, double y1, double zMin, double zMax, Fn fn)
{
   std::vector<Cell> cells;
   CellsInRect(x0, y0, x1, y1, cells);
   const double blockReach = DYE_BLOCK_UM / 2 + DYE_REACH_UM;
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
            fn(GetDyeBlock(A, (int)i, b));
         }
      }
   }
}

World::DyeBlock& World::GetDyeBlock(CellAssets& A, int mtIndex, int block)
{
   const Cell& c = A.cell;
   const BlockKey key = { c.cx, c.cy, mtIndex, block };
   auto it = dyeIndex_.find(key);
   if (it != dyeIndex_.end()) {
      stats_.dyeBlockHits++;
      if (it->second != dyeLru_.begin()) dyeLru_.splice(dyeLru_.begin(), dyeLru_, it->second);
      dyeLru_.front().second.used = query_;
      return dyeLru_.front().second;
   }
   DyeBlock blk;
   std::vector<Dye> dyes;
   DyesInBlock(seed_, c.cx, c.cy, mtIndex, A.mts[mtIndex].pts, A.Frames(mtIndex), block, p_.labelEfficiency,
               p_.labelNonBleaching, dyes);
   blk.dyes.reserve(dyes.size());
   blk.zLo = INFINITY; blk.zHi = -INFINITY;
   for (const Dye& d : dyes) {
      double wx, wy;
      LocalToWorld(c, d.pos.x, d.pos.y, wx, wy);
      blk.dyes.push_back({ wx, wy, d.pos.z, d.id, c.cx, c.cy, d.mtIndex, d.k, d.n, d.persistent });
      blk.zLo = std::min(blk.zLo, d.pos.z);
      blk.zHi = std::max(blk.zHi, d.pos.z);
   }
   blk.used = query_;
   blk.phase = Pcg4d((uint32_t)c.cx, (uint32_t)c.cy, (uint32_t)mtIndex, (uint32_t)block).a;
   stats_.dyeBlocks++;
   // Evict least recently used blocks, but never one this query uses (they
   // are the most recent ones): a window holding more dyes than the cap would
   // otherwise evict its own blocks and regenerate them on every query.
   dyeCount_ += blk.dyes.size();
   while (dyeCount_ > dyeCap_ && !dyeLru_.empty() && dyeLru_.back().second.used != query_) {
      dyeCount_ -= dyeLru_.back().second.dyes.size();
      dyeIndex_.erase(dyeLru_.back().first);
      dyeLru_.pop_back();
   }
   dyeLru_.emplace_front(key, std::move(blk));
   dyeIndex_[key] = dyeLru_.begin();
   return dyeLru_.front().second;
}

void World::Schedule(DyeBlock& b)
{
   if (b.scheduled) return;
   b.events.clear();
   b.persistent.clear();
   b.maxOn = 0;
   std::vector<Blink> blinks;
   uint32_t h1 = 0;
   int32_t lastCx = 0, lastCy = 0, lastMt = -1;
   for (size_t i = 0; i < b.dyes.size(); i++) {
      const WorldDye& d = b.dyes[i];
      if (d.persistent) { b.persistent.push_back((uint32_t)i); continue; }
      if (d.mtIndex != lastMt || d.cx != lastCx || d.cy != lastCy) {
         h1 = DyeH1(seed_, d.cx, d.cy, d.mtIndex);
         lastCx = d.cx; lastCy = d.cy; lastMt = d.mtIndex;
      }
      blinks.clear();
      DyeSchedule(h1, d.k, d.n, kin_, blinks);
      for (const Blink& bl : blinks) {
         b.events.push_back({ d.x, d.y, d.z, bl.tOn, bl.tOff, bl.brightness, d.id });
         b.maxOn = std::max(b.maxOn, bl.tOff - bl.tOn);
      }
   }
   // Stable order: by tOn, ties by dye order (deterministic either way).
   std::stable_sort(b.events.begin(), b.events.end(),
                    [](const WorldEvent& a, const WorldEvent& e) { return a.tOn < e.tOn; });
   b.scheduled = true;
   stats_.schedulesBuilt++;
}

void World::PersistentCover(DyeBlock& b, long b0, long b1)
{
   if (b0 >= b.pBin0 && b1 < b.pBin1) return;
   // Build a range of bins at once, up to 16 or ~2048 expected blinks, and
   // end it on a per-block phase so that the blocks of a window do not all
   // rebuild on the same query.
   const double perBin = (double)b.persistent.size() * kin_.activationRatePerSec * PERSIST_BIN_SEC;
   const long L = (long)std::min(16.0, std::max(1.0, std::floor(2048.0 / std::max(perBin, 1e-9))));
   const long end = b1 + 1 + (L - (long)((b1 + 1 + (long)(b.phase % (uint32_t)L)) % L)) % L;
   b.pEvents.clear();
   std::vector<BinBlink> bl;
   uint32_t h1 = 0;
   int32_t lastCx = 0, lastCy = 0, lastMt = -1;
   for (uint32_t i : b.persistent) {
      const WorldDye& d = b.dyes[i];
      if (d.mtIndex != lastMt || d.cx != lastCx || d.cy != lastCy) {
         h1 = DyeH1(seed_, d.cx, d.cy, d.mtIndex);
         lastCx = d.cx; lastCy = d.cy; lastMt = d.mtIndex;
      }
      bl.clear();
      PersistentBlinksInBins(h1, d.k, d.n, kin_, b0, end - 1, bl);
      for (const BinBlink& e : bl) b.pEvents.push_back({ e.tOn, e.tOff, e.brightness, i, e.bin, e.j });
   }
   std::sort(b.pEvents.begin(), b.pEvents.end(),
             [](const PersistentEvent& a, const PersistentEvent& e) { return a.tOn < e.tOn; });
   b.pBin0 = b0;
   b.pBin1 = end;
   stats_.persistentBuilt++;
}

void World::SetKinetics(const Kinetics& k)
{
   kin_ = k;
   for (auto& e : dyeLru_) {
      e.second.scheduled = false;
      std::vector<WorldEvent>().swap(e.second.events);
      std::vector<PersistentEvent>().swap(e.second.pEvents);
      e.second.pBin0 = e.second.pBin1 = 0;
   }
}

bool World::FindCell(int32_t cx, int32_t cy, Cell& out)
{
   for (const Cell& c : PackedBlock(FloorDiv(cx, PACK_BLOCK_CHUNKS), FloorDiv(cy, PACK_BLOCK_CHUNKS)))
      if (c.cx == cx && c.cy == cy) { out = c; return true; }
   return false;
}

void World::SitesInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                          std::vector<WorldDye>& out)
{
   query_++;
   ForEachDyeBlock(x0, y0, x1, y1, zMin, zMax, [&](DyeBlock& b) {
      if (b.dyes.empty() || b.zHi < zMin || b.zLo >= zMax) return;
      for (const WorldDye& d : b.dyes)
         if (d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) out.push_back(d);
   });
}

void World::EventsInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                           double t0, double t1, std::vector<WorldEvent>& out)
{
   query_++;
   ForEachDyeBlock(x0, y0, x1, y1, zMin, zMax, [&](DyeBlock& b) {
      if (b.dyes.empty() || b.zHi < zMin || b.zLo >= zMax) return;
      Schedule(b);
      // Only blinks with tOn in [t0 - maxOn, t1) can overlap [t0, t1).
      auto it = std::lower_bound(b.events.begin(), b.events.end(), t0 - b.maxOn,
                                 [](const WorldEvent& e, double t) { return e.tOn < t; });
      for (; it != b.events.end() && it->tOn < t1; ++it) {
         const WorldEvent& e = *it;
         if (e.tOff > t0 && e.z >= zMin && e.z < zMax && e.x >= x0 && e.x < x1 && e.y >= y0 && e.y < y1)
            out.push_back(e);
      }
      // Persistent sites never bleach, so their blinks are addressed per time
      // bin (see PersistentBlinks) and cached for a range of bins. The answer
      // is PersistentBlinks' for every site in the window, in its order
      // (site, bin, j).
      if (b.persistent.empty() || !(kin_.activationRatePerSec > 0) || !(t1 > t0)) return;
      const double maxOn = PERSIST_ON_CAP * kin_.onSec;
      const long b0 = std::max(0L, (long)std::floor((t0 - maxOn) / PERSIST_BIN_SEC));
      const long b1 = (long)std::floor(t1 / PERSIST_BIN_SEC);
      PersistentCover(b, b0, b1);
      std::vector<const PersistentEvent*>& hit = persistentScratch_;
      hit.clear();
      auto p = std::lower_bound(b.pEvents.begin(), b.pEvents.end(), b0 * PERSIST_BIN_SEC,
                                [](const PersistentEvent& e, double t) { return e.tOn < t; });
      for (; p != b.pEvents.end() && p->tOn < t1; ++p) {
         if (p->bin < (uint32_t)b0 || !(p->tOff > t0)) continue;
         const WorldDye& d = b.dyes[p->dye];
         if (d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) hit.push_back(&*p);
      }
      std::sort(hit.begin(), hit.end(), [](const PersistentEvent* a, const PersistentEvent* e) {
         return a->dye != e->dye ? a->dye < e->dye : a->bin != e->bin ? a->bin < e->bin : a->j < e->j;
      });
      for (const PersistentEvent* e : hit) {
         const WorldDye& d = b.dyes[e->dye];
         out.push_back({ d.x, d.y, d.z, e->tOn, e->tOff, e->brightness, d.id });
      }
   });
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
