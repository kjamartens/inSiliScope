#include "world.h"

#include "jsmath.h"
#include "packing.h"
#include "parallel.h"

#include <algorithm>
#include <chrono>
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
   return 2 * worstSemiMajor * CELL_MOD_MAX * (p.cellRough > 0 && p.cellBlob > 0 ? 1 + TAIL_MAX : 1) + p.chunkSize;
}

// A cell's geometry and microtubules (a pure function of seed, cell, params).
std::unique_ptr<CellAssets> BuildCellAssets(uint32_t seed, const Cell& c, const Params& p)
{
   std::unique_ptr<CellAssets> a(new CellAssets());
   a->cell = c;
   a->geom = BuildMtCellGeom(c, p);
   a->mts = BuildMicrotubulesForCell(seed, c, p, a->geom);
   a->mtReach.resize(a->mts.size());
   a->frames.resize(a->mts.size());
   for (size_t i = 0; i < a->mts.size(); i++) {
      double r = 0;
      for (const Pt3& q : a->mts[i].pts) r = std::max(r, jsm::hypot(q.x, q.y));
      a->mtReach[i] = r;
   }
   return a;
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
   prefetchDone_.valid = false;
}

std::vector<Cell> World::PackBlock(int32_t bx, int32_t by) const
{
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
   return cells;
}

const std::vector<Cell>& World::PackedBlock(int32_t bx, int32_t by)
{
   const auto key = std::make_pair(bx, by);
   auto it = blocks_.find(key);
   if (it != blocks_.end()) return it->second;
   if (blocks_.size() >= BLOCK_CACHE_MAX) blocks_.clear();
   std::vector<Cell> cells;
   auto pre = packPrebuilt_.find(key);
   if (pre != packPrebuilt_.end()) {
      cells = std::move(pre->second);
      packPrebuilt_.erase(pre);
   } else {
      cells = PackBlock(bx, by);
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
   // Pack the missing blocks in parallel (each is a pure function of its
   // address); PackedBlock below takes them in the usual order.
   std::vector<std::pair<int32_t, int32_t>> missing;
   for (int32_t bx = bx0; bx <= bx1; bx++)
      for (int32_t by = by0; by <= by1; by++)
         if (!blocks_.count(std::make_pair(bx, by))) missing.push_back(std::make_pair(bx, by));
   if (missing.size() > 1) {
      std::vector<std::vector<Cell>> built(missing.size());
      ParallelFor(missing.size(), 1, [&](size_t i) { built[i] = PackBlock(missing[i].first, missing[i].second); });
      for (size_t i = 0; i < missing.size(); i++) packPrebuilt_[missing[i]] = std::move(built[i]);
   }
   for (int32_t bx = bx0; bx <= bx1; bx++)
      for (int32_t by = by0; by <= by1; by++)
         for (const Cell& c : PackedBlock(bx, by))
            if (RectDist(c.x, c.y, x0, y0, x1, y1) <= c.rOuter) out.push_back(c);
   packPrebuilt_.clear();
}

CellAssets& World::Assets(const Cell& c)
{
   const auto key = std::make_pair(c.cx, c.cy);
   for (auto it = assets_.begin(); it != assets_.end(); ++it) {
      if (it->first != key) continue;
      if (it != assets_.begin()) assets_.splice(assets_.begin(), assets_, it);
      return *assets_.front().second;
   }
   std::unique_ptr<CellAssets> a;
   auto pre = assetPrebuilt_.find(key);
   if (pre != assetPrebuilt_.end()) {
      a = std::move(pre->second);
      assetPrebuilt_.erase(pre);
   } else {
      a = BuildCellAssets(seed_, c, p_);
   }
   stats_.cellsBuilt++;
   assets_.emplace_front(key, std::move(a));
   while (assets_.size() > assetCap_) assets_.pop_back();
   return *assets_.front().second;
}

void World::PrebuildAssets(const std::vector<Cell>& cells)
{
   std::vector<const Cell*> missing;
   for (const Cell& c : cells) {
      const auto key = std::make_pair(c.cx, c.cy);
      bool known = assetPrebuilt_.count(key) > 0;
      for (auto it = assets_.begin(); !known && it != assets_.end(); ++it) known = it->first == key;
      if (!known) {
         missing.push_back(&c);
         assetPrebuilt_[key] = nullptr;
      }
   }
   assetPrebuilt_.clear();
   if (missing.size() > 1) {
      std::vector<std::unique_ptr<CellAssets>> built(missing.size());
      ParallelFor(missing.size(), 1, [&](size_t i) { built[i] = BuildCellAssets(seed_, *missing[i], p_); });
      for (size_t i = 0; i < missing.size(); i++)
         assetPrebuilt_[std::make_pair(missing[i]->cx, missing[i]->cy)] = std::move(built[i]);
   }
}

template <class Fn>
void World::ForEachDyeBlock(double x0, double y0, double x1, double y1, double zMin, double zMax,
                            const BlockPrep& prep, const BlockNeeds& needs, Fn fn)
{
   query_++;
   std::vector<Cell> cells;
   CellsInRect(x0, y0, x1, y1, cells);
   // The assets of the cells not cached, built in parallel (Assets takes them
   // in the usual order). Not under a deadline: that walk may stop early.
   if (!deadline_) PrebuildAssets(cells);
   const double blockReach = DYE_BLOCK_UM / 2 + DYE_REACH_UM;
   struct Item { DyeBlock* b; int mt, block; };
   std::vector<Item> batch;
   std::vector<size_t> work;
   for (const Cell& c : cells) {
      if (PastDeadline()) break;
      CellAssets& A = Assets(c);
      const double centreDist = RectDist(c.x, c.y, x0, y0, x1, y1);
      batch.clear();
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
            if (PastDeadline()) break;
            batch.push_back({ &FindDyeBlock(c, (int)i, b), (int)i, b });
         }
      }
      // This cell's new dye blocks, then the prep work, in parallel; each
      // item touches only its own block.
      work.clear();
      for (size_t k = 0; k < batch.size(); k++)
         if (!batch[k].b->generated) work.push_back(k);
      ParallelFor(work.size(), 2, [&](size_t w) {
         const Item& it = batch[work[w]];
         GenerateDyes(*it.b, c, A.mts[it.mt].pts, *A.frames[it.mt], it.mt, it.block);
      });
      for (size_t k : work) dyeCount_ += batch[k].b->dyes.size();
      if (prep) {
         work.clear();
         for (size_t k = 0; k < batch.size(); k++)
            if (needs(*batch[k].b)) work.push_back(k);
         ParallelFor(work.size(), 2, [&](size_t w) { prep(*batch[work[w]].b); });
      }
      for (const Item& it : batch) fn(*it.b);
   }
   assetPrebuilt_.clear();
   // Evict least recently used blocks only now, and never one this query
   // used (they are the most recent ones): evicting while the query runs
   // could drop blocks it has not reached yet and regenerate them, and a
   // window holding more dyes than the cap would do so on every query.
   while (dyeCount_ > dyeCap_ && !dyeLru_.empty() && dyeLru_.back().second.used != query_) {
      dyeCount_ -= dyeLru_.back().second.dyes.size();
      dyeIndex_.erase(dyeLru_.back().first);
      dyeLru_.pop_back();
      evictions_++;
   }
}

bool World::PastDeadline()
{
   if (!deadline_) return false;
   if (!stopped_ && std::chrono::steady_clock::now() >= *deadline_) stopped_ = true;
   return stopped_;
}

bool World::Prefetch(double x0, double y0, double x1, double y1, double zMin, double zMax, double t0, double t1,
                     double budgetMs)
{
   // Nothing new since the last complete prefetch of a region holding this one.
   const PrefetchRegion r = { x0, y0, x1, y1, zMin, zMax, evictions_, kinVersion_, true };
   const PrefetchRegion& d = prefetchDone_;
   if (d.valid && x0 >= d.x0 && y0 >= d.y0 && x1 <= d.x1 && y1 <= d.y1 && zMin >= d.zMin && zMax <= d.zMax &&
       d.evictions == evictions_ && d.kinVersion == kinVersion_)
      return true;
   const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double, std::milli>(std::max(0.0, budgetMs)));
   deadline_ = &deadline;
   stopped_ = false;
   const bool persist = kin_.activationRatePerSec > 0 && t1 > t0;
   const double maxOn = PERSIST_ON_CAP * kin_.onSec;
   const long b0 = std::max(0L, (long)std::floor((t0 - maxOn) / PERSIST_BIN_SEC));
   const long b1 = (long)std::floor(t1 / PERSIST_BIN_SEC);
   PrepCounts n;
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax,
      [&](DyeBlock& b) {
         if (Schedule(b)) n.schedules++;
         if (persist && !b.persistent.empty() && PersistentCover(b, b0, b1, t1, false)) n.covers++;
      },
      [&](const DyeBlock& b) {
         return !b.dyes.empty() && (!b.scheduled || (persist && !b.persistent.empty() && CoverWouldBuild(b, b0, b1, t1)));
      },
      [](DyeBlock&) {});
   stats_.schedulesBuilt += n.schedules.load();
   stats_.persistentBuilt += n.covers.load();
   deadline_ = nullptr;
   const bool complete = !stopped_;
   stopped_ = false;
   if (complete) {
      // Whatever this pass evicted lay outside the region (it keeps the
      // blocks it used), so count from after it.
      prefetchDone_ = r;
      prefetchDone_.evictions = evictions_;
      prefetchDone_.valid = true;
   }
   stats_.prefetches++;
   return complete;
}

World::DyeBlock& World::FindDyeBlock(const Cell& c, int mtIndex, int block)
{
   const BlockKey key = { c.cx, c.cy, mtIndex, block };
   auto it = dyeIndex_.find(key);
   if (it != dyeIndex_.end()) {
      stats_.dyeBlockHits++;
      if (it->second != dyeLru_.begin()) dyeLru_.splice(dyeLru_.begin(), dyeLru_, it->second);
      dyeLru_.front().second.used = query_;
      return dyeLru_.front().second;
   }
   // Filled in by GenerateDyes before anything reads it (ForEachDyeBlock),
   // counted in dyeCount_ then; evicted only at the end of a query.
   DyeBlock blk;
   blk.used = query_;
   stats_.dyeBlocks++;
   dyeLru_.emplace_front(key, std::move(blk));
   dyeIndex_[key] = dyeLru_.begin();
   return dyeLru_.front().second;
}

void World::GenerateDyes(DyeBlock& blk, const Cell& c, const std::vector<Pt3>& pts, const MtFrames& fr, int mtIndex,
                         int block) const
{
   std::vector<Dye> dyes;
   // ~1625 lattice sites per block; the labelled fraction of them.
   dyes.reserve((size_t)(1625 * std::min(1.0, std::max(0.0, p_.labelEfficiency) + std::max(0.0, p_.labelNonBleaching))) + 16);
   DyesInBlock(seed_, c.cx, c.cy, mtIndex, pts, fr, block, p_.labelEfficiency, p_.labelNonBleaching, dyes);
   blk.dyes.reserve(dyes.size());
   blk.zLo = INFINITY; blk.zHi = -INFINITY;
   // LocalToWorld's cos/sin of packRot once per block, not per dye (its branch kept).
   const double rot = c.packRot;
   const bool rotated = !(rot == 0 || std::isnan(rot));
   const double cr = rotated ? jsm::cos(rot) : 1, sr = rotated ? jsm::sin(rot) : 0;
   for (const Dye& d : dyes) {
      double wx, wy;
      if (!rotated) { wx = c.x + d.pos.x; wy = c.y + d.pos.y; }
      else { wx = c.x + d.pos.x * cr - d.pos.y * sr; wy = c.y + d.pos.x * sr + d.pos.y * cr; }
      blk.dyes.push_back({ wx, wy, d.pos.z, d.id, c.cx, c.cy, d.mtIndex, d.k, d.n, d.persistent });
      blk.zLo = std::min(blk.zLo, d.pos.z);
      blk.zHi = std::max(blk.zHi, d.pos.z);
   }
   blk.phase = Pcg4d((uint32_t)c.cx, (uint32_t)c.cy, (uint32_t)mtIndex, (uint32_t)block).a;
   blk.generated = true;
}

bool World::Schedule(DyeBlock& b) const
{
   if (b.scheduled) return false;
   b.events.clear();
   b.persistent.clear();
   b.maxOn = 0;
   b.events.reserve(b.dyes.size());
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
   return true;
}

bool World::CoverWouldBuild(const DyeBlock& b, long b0, long b1, double t1) const
{
   if (b0 < b.pBin0 || b0 >= b.pBin1) return true;
   const double frac = b.phase * (1.0 / 4294967296.0);
   return !(b1 < b.pBin1 && t1 < (b.pBin1 - 1 + frac) * PERSIST_BIN_SEC);
}

bool World::PersistentCover(DyeBlock& b, long b0, long b1, double t1, bool shortFirst) const
{
   // Bins per build: up to 16 or ~2048 expected blinks.
   const double perBin = (double)b.persistent.size() * kin_.activationRatePerSec * PERSIST_BIN_SEC;
   const long L = (long)std::min(16.0, std::max(1.0, std::floor(2048.0 / std::max(perBin, 1e-9))));
   long lo, hi;
   if (b0 < b.pBin0 || b0 >= b.pBin1) {
      // First use, a jump in time or new kinetics: build from scratch --
      // for a query (shortFirst) only one bin ahead, so a window full of new
      // blocks (a stage jump, a kinetics change) builds what it shows now
      // and the rest in the staggered extensions below.
      b.pEvents.clear();
      b.pBin0 = lo = b0;
      hi = b1 + 1 + (shortFirst ? 1 : L);
   } else {
      // Extend ahead of time, once t1 passes a per-block point in the last
      // bin covered: the blocks of a window then spread their builds over
      // that bin's frames instead of all building on the frame that enters
      // the next bin (a stall every PERSIST_BIN_SEC).
      const double frac = b.phase * (1.0 / 4294967296.0);
      if (b1 < b.pBin1 && t1 < (b.pBin1 - 1 + frac) * PERSIST_BIN_SEC) return false;
      // Drop the bins the lookback no longer reaches (tOn, hence bin, ascends).
      b.pEvents.erase(b.pEvents.begin(),
                      std::partition_point(b.pEvents.begin(), b.pEvents.end(),
                                           [&](const PersistentEvent& e) { return (long)e.bin < b0; }));
      b.pBin0 = b0;
      lo = b.pBin1;
      hi = std::max(b1 + 1, b.pBin1) + L;
   }
   const size_t start = b.pEvents.size();
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
      PersistentBlinksInBins(h1, d.k, d.n, kin_, lo, hi - 1, bl);
      for (const BinBlink& e : bl) b.pEvents.push_back({ e.tOn, e.tOff, e.brightness, i, e.bin, e.j });
   }
   // The new bins all start after the kept ones, so sorting them keeps the
   // whole list sorted by tOn.
   std::sort(b.pEvents.begin() + start, b.pEvents.end(),
             [](const PersistentEvent& a, const PersistentEvent& e) { return a.tOn < e.tOn; });
   b.pBin1 = hi;
   return true;
}

void World::SetKinetics(const Kinetics& k)
{
   kin_ = k;
   kinVersion_++;
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
   ForEachDyeBlock(x0, y0, x1, y1, zMin, zMax, nullptr, nullptr, [&](DyeBlock& b) {
      if (b.dyes.empty() || b.zHi < zMin || b.zLo >= zMax) return;
      for (const WorldDye& d : b.dyes)
         if (d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) out.push_back(d);
   });
}

void World::EventsInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                           double t0, double t1, std::vector<WorldEvent>& out)
{
   // Blocks the window can see: their schedules and persistent blinks are
   // built (in parallel) before the serial pass below reads them.
   const bool persist = kin_.activationRatePerSec > 0 && t1 > t0;
   const double maxOn = PERSIST_ON_CAP * kin_.onSec;
   const long b0 = std::max(0L, (long)std::floor((t0 - maxOn) / PERSIST_BIN_SEC));
   const long b1 = (long)std::floor(t1 / PERSIST_BIN_SEC);
   auto visible = [&](const DyeBlock& b) { return !b.dyes.empty() && !(b.zHi < zMin || b.zLo >= zMax); };
   PrepCounts n;
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax,
      [&](DyeBlock& b) {
         if (Schedule(b)) n.schedules++;
         if (persist && !b.persistent.empty() && PersistentCover(b, b0, b1, t1, true)) n.covers++;
      },
      [&](const DyeBlock& b) {
         return visible(b) && (!b.scheduled || (persist && !b.persistent.empty() && CoverWouldBuild(b, b0, b1, t1)));
      },
      [&](DyeBlock& b) {
         if (!visible(b)) return;
         // Built above; these only check (a block the prep skipped needs nothing).
         if (Schedule(b)) n.schedules++;
         if (persist && !b.persistent.empty() && PersistentCover(b, b0, b1, t1, true)) n.covers++;
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
         if (b.persistent.empty() || !persist) return;
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
   stats_.schedulesBuilt += n.schedules.load();
   stats_.persistentBuilt += n.covers.load();
}

long World::DensityInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                            int nx, int ny, float* out)
{
   // The nz = 1, both-populations case of Density3dInWindow: the same dyes
   // (SitesInWindow's filter), the same bins, integer counts in any order --
   // without materialising the site list.
   return Density3dInWindow(x0, y0, x1, y1, zMin, zMax, nx, ny, 1, 3u, out);
}

long World::Density3dInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                              int nx, int ny, int nz, unsigned populations, float* out)
{
   std::fill(out, out + (size_t)nx * ny * nz, 0.0f);
   const bool wantBleach = (populations & 1u) != 0, wantPersist = (populations & 2u) != 0;
   if (!wantBleach && !wantPersist) return 0;
   // Same binning as DensityInWindow, so nz = 1 reproduces it.
   const double sx = nx / (x1 - x0), sy = ny / (y1 - y0);
   const double sz = nz > 1 ? nz / (zMax - zMin) : 0.0;
   long total = 0;
   ForEachDyeBlock(x0, y0, x1, y1, zMin, zMax, nullptr, nullptr, [&](DyeBlock& b) {
      if (b.dyes.empty() || b.zHi < zMin || b.zLo >= zMax) return;
      for (const WorldDye& d : b.dyes) {
         if (!(d.persistent ? wantPersist : wantBleach)) continue;
         if (!(d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1)) continue;
         const int ix = std::min(nx - 1, (int)std::floor((d.x - x0) * sx));
         const int iy = std::min(ny - 1, (int)std::floor((d.y - y0) * sy));
         const int iz = nz > 1 ? std::min(nz - 1, (int)std::floor((d.z - zMin) * sz)) : 0;
         out[((size_t)iz * ny + iy) * nx + ix] += 1;
         total++;
      }
   });
   return total;
}


namespace {
double Overlap(double a0, double a1, double b0, double b1)
{
   return std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
}
} // namespace

long World::OpticalVolumeInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                                  int nx, int ny, int nz, int sub, float* out)
{
   const size_t plane = (size_t)nx * ny, chan = plane * nz;
   std::fill(out, out + chan * 3, 0.0f);
   std::vector<Cell> cells;
   CellsInRect(x0, y0, x1, y1, cells);
   PrebuildAssets(cells);
   const double px = (x1 - x0) / nx, py = (y1 - y0) / ny, dz = (zMax - zMin) / nz;
   const double subW = 1.0 / ((double)sub * sub);
   const double mtArea = jsm::PI * (MT_RADIUS_NM / 1000) * (MT_RADIUS_NM / 1000);
   const double mtStep = 0.5 * std::min(px, std::min(py, dz));
   for (const Cell& c : cells) {
      CellAssets& A = Assets(c);
      const CytoMesh& mesh = A.geom.mesh;
      const bool rotated = !(c.packRot == 0 || std::isnan(c.packRot));
      const double cr = rotated ? jsm::cos(c.packRot) : 1, sr = rotated ? jsm::sin(c.packRot) : 0;
      const double ncr = jsm::cos(-c.nucRot), nsr = jsm::sin(-c.nucRot);
      const double na = std::max(1e-6, c.nucLong / 2), nb = std::max(1e-6, c.nucShort / 2);
      const double nrz = c.nucHeight / 2;
      const double rIn = CellInnerRadiusBound(c);
      const int ix0 = std::max(0, (int)std::floor((c.x - c.rOuter - x0) / px));
      const int ix1 = std::min(nx - 1, (int)std::floor((c.x + c.rOuter - x0) / px));
      const int iy0 = std::max(0, (int)std::floor((c.y - c.rOuter - y0) / py));
      const int iy1 = std::min(ny - 1, (int)std::floor((c.y + c.rOuter - y0) / py));
      if (ix0 <= ix1 && iy0 <= iy1) {
         // Rows in parallel: each writes only its own row of every plane.
         ParallelFor((size_t)(iy1 - iy0 + 1), 4, [&](size_t r) {
            const int iy = iy0 + (int)r;
            for (int ix = ix0; ix <= ix1; ix++) {
               for (int sv = 0; sv < sub; sv++)
                  for (int su = 0; su < sub; su++) {
                     const double dx = x0 + (ix + (su + 0.5) / sub) * px - c.x;
                     const double dy = y0 + (iy + (sv + 0.5) / sub) * py - c.y;
                     const double lx = dx * cr + dy * sr, ly = -dx * sr + dy * cr;
                     const double rr = jsm::hypot(lx, ly);
                     if (rr > c.rOuter || (rr > rIn && rr > CellRadiusAt(c, jsm::atan2(ly, lx)))) continue;
                     const double h = SampleCytoMeshHeight(c, mesh, lx, ly);
                     if (!(h > 0)) continue;
                     // Nucleus chord through this column, clipped to the body.
                     const double ex = lx - c.nucOffX, ey = ly - c.nucOffY;
                     const double ux = (ex * ncr - ey * nsr) / na, uy = (ex * nsr + ey * ncr) / nb;
                     const double q = 1 - ux * ux - uy * uy;
                     double zn0 = 0, zn1 = 0;
                     if (q > 0 && nrz > 0) {
                        const double half = nrz * std::sqrt(q);
                        zn0 = std::max(0.0, c.nucZ - half);
                        zn1 = std::min(h, c.nucZ + half);
                        if (zn1 < zn0) zn0 = zn1 = 0;
                     }
                     const int k0 = std::max(0, (int)std::floor((0 - zMin) / dz));
                     const int k1 = std::min(nz - 1, (int)std::floor((h - zMin) / dz));
                     for (int k = k0; k <= k1; k++) {
                        const double zl = zMin + k * dz, zh = zl + dz;
                        const double nuc = Overlap(zl, zh, zn0, zn1);
                        const double body = Overlap(zl, zh, 0, h);
                        const size_t v = ((size_t)k * ny + iy) * nx + ix;
                        out[v] += (float)((body - nuc) / dz * subW);
                        out[chan + v] += (float)(nuc / dz * subW);
                     }
                  }
            }
         });
      }
      // Microtubules: their tube volume, deposited along the centrelines.
      const double voxVol = px * py * dz;
      for (const Microtubule& m : A.mts) {
         for (size_t i = 0; i + 1 < m.pts.size(); i++) {
            const Pt3& a = m.pts[i];
            const Pt3& b = m.pts[i + 1];
            const double len = jsm::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
            if (!(len > 0)) continue;
            const int n = std::max(1, (int)std::ceil(len / mtStep));
            for (int j = 0; j < n; j++) {
               const double t = (j + 0.5) / n;
               const double lx = a.x + (b.x - a.x) * t, ly = a.y + (b.y - a.y) * t, z = a.z + (b.z - a.z) * t;
               const double wx = c.x + lx * cr - ly * sr, wy = c.y + lx * sr + ly * cr;
               if (!(wx >= x0 && wx < x1 && wy >= y0 && wy < y1 && z >= zMin && z < zMax)) continue;
               const int ix = std::min(nx - 1, (int)std::floor((wx - x0) / px));
               const int iy = std::min(ny - 1, (int)std::floor((wy - y0) / py));
               const int k = std::min(nz - 1, (int)std::floor((z - zMin) / dz));
               out[2 * chan + ((size_t)k * ny + iy) * nx + ix] += (float)(len / n * mtArea / voxVol);
            }
         }
      }
   }
   assetPrebuilt_.clear();
   return (long)cells.size();
}

} // namespace isc
