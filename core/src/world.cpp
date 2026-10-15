#include "world.h"

#include "jsmath.h"
#include "packing.h"
#include "parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace isc {

namespace {

constexpr size_t BLOCK_CACHE_MAX = 4096;

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

// A dose clock's kinetics history for a dye at (x, y, z) (ABI 14): epoch e >= 1 from its dose bound[e - 1] there; an
// epoch the dye had no dose in gives way to the next (the later kinetics from the same dose).
void DoseHistory(const ClockMap& m, const StructClock& c, double x, double y, double z, KineticsHistory& out)
{
   out.tStart.assign(1, 0.0);
   out.kin.assign(1, c.hist.kin[0]);
   for (size_t e = 1; e < c.hist.Size(); e++) {
      const double B = m.Eval(c.bound[e - 1], x, y, z);
      if (B > out.tStart.back()) {
         out.tStart.push_back(B);
         out.kin.push_back(c.hist.kin[e]);
      } else {
         out.kin.back() = c.hist.kin[e];
      }
   }
}

// Upper bound on how far a cell's footprint reaches from its home chunk:
// the worst-case rOuter (as InteractionChunks) twice over, plus a chunk for
// the packing displacement.
double CellReachUm(const Params& p)
{
   const double worstSemiMajor = (p.cellDiamMax / 2) / jsm::sqrt(std::max(0.05, p.cellElongMin));
   return 2 * worstSemiMajor * CELL_MOD_MAX * (p.cellRough > 0 && p.cellBlob > 0 ? 1 + TAIL_MAX : 1) + p.chunkSize;
}

// The microtubules of a cell's assets (and their reach), on demand.
void EnsureMts(uint32_t seed, CellAssets& a, const Params& p)
{
   if (a.mtsBuilt) return;
   a.mts = BuildMicrotubulesForCell(seed, a.cell, p, a.geom);
   a.mtReach.resize(a.mts.size());
   a.frames.resize(a.mts.size());
   a.mids.clear();
   for (size_t i = 0; i < a.mts.size(); i++) {
      double r = 0;
      for (const Pt3& q : a.mts[i].pts) r = std::max(r, jsm::hypot(q.x, q.y));
      a.mtReach[i] = r;
   }
   a.mtsBuilt = true;
}

// The nuclear pores of a cell's assets and their dye blocks, on demand (JS world.js npcs()): the pores per
// NPC_BLOCK_UM cube, ordered (iz, iy, ix), pores in candidate order within a block.
void EnsureNpcs(uint32_t seed, CellAssets& a, const Params& p)
{
   if (a.npcsBuilt) return;
   const Cell& c = a.cell;
   a.npcs = BuildNpcsForCell(seed, c.cx, c.cy, c, p);
   std::map<std::array<int32_t, 3>, size_t> cubes;   // (iz, iy, ix) -> block: the map's order is the blocks' order
   std::vector<CellAssets::NpcBlock> blocks;
   double reachXY = 0;
   for (size_t i = 0; i < a.npcs.size(); i++) {
      const Npc& q = a.npcs[i];
      const int32_t ix = (int32_t)std::floor(q.c.x / NPC_BLOCK_UM), iy = (int32_t)std::floor(q.c.y / NPC_BLOCK_UM);
      const int32_t iz = (int32_t)std::floor(q.c.z / NPC_BLOCK_UM);
      auto it = cubes.find({ iz, iy, ix });
      if (it == cubes.end()) {
         it = cubes.emplace(std::array<int32_t, 3>{ iz, iy, ix }, blocks.size()).first;
         CellAssets::NpcBlock b;
         b.ix = ix; b.iy = iy; b.iz = iz;
         blocks.push_back(b);
      }
      blocks[it->second].ids.push_back((int32_t)i);
      reachXY = std::max(reachXY, jsm::hypot(q.c.x, q.c.y));
   }
   a.npcBlocks.clear();
   a.npcBlocks.reserve(blocks.size());
   for (const auto& kv : cubes) {
      CellAssets::NpcBlock b = std::move(blocks[kv.second]);
      b.x = (b.ix + 0.5) * NPC_BLOCK_UM;
      b.y = (b.iy + 0.5) * NPC_BLOCK_UM;
      b.z = (b.iz + 0.5) * NPC_BLOCK_UM;
      LocalToWorld(c, b.x, b.y, b.wx, b.wy);
      a.npcBlocks.push_back(std::move(b));
   }
   a.npcReachXY = reachXY;
   NpcAttachmentPoints(p, a.npcSites);
   a.npcsBuilt = true;
}

// A cell's geometry and (withMts) microtubules (withNpcs: and pores): a pure function of seed, cell, params.
std::unique_ptr<CellAssets> BuildCellAssets(uint32_t seed, const Cell& c, const Params& p, bool withMts,
                                            bool withNpcs = false)
{
   std::unique_ptr<CellAssets> a(new CellAssets());
   a->cell = c;
   a->geom = BuildMtCellGeom(c, p);
   if (withMts) EnsureMts(seed, *a, p);
   if (withNpcs) EnsureNpcs(seed, *a, p);
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

const std::vector<CellAssets::BlockMid>& CellAssets::Mids(size_t i)
{
   if (mids.size() != mts.size()) mids.resize(mts.size());
   std::vector<BlockMid>& m = mids[i];
   if (m.empty() && mts[i].pts.size() >= 2) {
      const std::vector<Pt3>& pts = mts[i].pts;
      const MtFrames& fr = Frames(i);
      const double len = fr.Length();
      const int nBlocks = (int)std::ceil(len / DYE_BLOCK_UM);
      m.reserve((size_t)std::max(0, nBlocks));
      for (int b = 0; b < nBlocks; b++) {
         // Every point of block b lies within half a block (arc) of its midpoint.
         const Pt3 mid = PointAtArc(pts, fr, std::min((b + 0.5) * DYE_BLOCK_UM, len));
         double wx, wy;
         LocalToWorld(cell, mid.x, mid.y, wx, wy);
         m.push_back({ wx, wy, mid.z });
      }
   }
   return m;
}

World::World(uint32_t seed, const Params& p, size_t assetCacheCells, size_t dyeCacheDyes)
   : seed_(seed), p_(p), assetCap_(std::max<size_t>(1, assetCacheCells)), dyeCap_(dyeCacheDyes),
     pool_(AcquireWorkerPool())
{
   NormalizeParams(p_);
   NormalizeOrigamiParams(p_);
   if ((int)p_.specimen == SPECIMEN_NANORULERS) origami_.reset(new OrigamiField(seed_, p_));
   for (int s = 0; s < STRUCTURE_COUNT; s++) {
      labels_[(size_t)s].linkage = NormLinkage(LegacyLinkage(s));
      labels_[(size_t)s].hasLinkage = true;
   }
}

bool World::NpcsLabelled() const
{
   const Label& l = labels_[STRUCTURE_NPC];
   return p_.npcDensity > 0 && l.density > 0 && l.fluorescentFraction > 0;
}

double World::MtDyeReach() const
{
   const Linkage& lk = labels_[STRUCTURE_MT].linkage;
   return (MT_RADIUS_NM + lk.binderNm + lk.binderFlexNm + lk.linkerMaxNm) * 1e-3;
}

double World::NpcDyeReach() const
{
   return NpcSiteReachUm(p_) + LinkageReachNm(labels_[STRUCTURE_NPC].linkage) * 1e-3;
}

World::~World()
{
   if (store_) store_->Flush();
}

bool World::SetCacheDir(const std::string& dir)
{
   if (store_) {
      store_->Flush();
      store_.reset();
   }
   if (dir.empty() || origami_) return true;   // the store holds packed cells: a cell field's only
   std::unique_ptr<BlockStore> s(new BlockStore());
   if (!s->Open(dir, BlockStoreKey(seed_, PackingFingerprint(p_)))) return false;
   store_ = std::move(s);
   return true;
}

bool World::FlushCache() { return store_ ? store_->Flush() : false; }

namespace {
// The store's rows of packed cells: cx, cy, x, y, packRot.
std::vector<double> StoreRows(const std::vector<Cell>& cells)
{
   std::vector<double> rows;
   rows.reserve(cells.size() * BLOCK_STORE_STRIDE);
   for (const Cell& c : cells) {
      rows.push_back(c.cx); rows.push_back(c.cy);
      rows.push_back(c.x); rows.push_back(c.y); rows.push_back(c.packRot);
   }
   return rows;
}
} // namespace

bool World::StoredBlock(int32_t bx, int32_t by, std::vector<Cell>& out)
{
   const std::vector<double>* rows = store_->Find(bx, by);
   if (!rows) return false;
   if (CellsFromRows(bx, by, rows->data(), (int32_t)(rows->size() / BLOCK_STORE_STRIDE), BLOCK_STORE_STRIDE, out))
      return true;
   store_->Erase(bx, by);   // not this world's cells (another core version, a damaged file): repack
   out.clear();
   return false;
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
   } else if (store_ && StoredBlock(bx, by, cells)) {
      stats_.blocksFromStore++;
      return blocks_.emplace(key, std::move(cells)).first->second;
   } else {
      cells = PackBlock(bx, by);
   }
   stats_.blocksPacked++;
   if (store_) store_->Put(bx, by, StoreRows(cells));
   return blocks_.emplace(key, std::move(cells)).first->second;
}

void World::CellsInRect(double x0, double y0, double x1, double y1, std::vector<Cell>& out)
{
   std::vector<const Cell*> ptrs;
   CellsInRectPtr(x0, y0, x1, y1, ptrs);
   out.reserve(out.size() + ptrs.size());
   for (const Cell* c : ptrs) out.push_back(*c);
}

void World::CellsInRectPtr(double x0, double y0, double x1, double y1, std::vector<const Cell*>& out)
{
   if (origami_) return;
   PoolScope scope(pool_);
   const double S = p_.chunkSize, reach = CellReachUm(p_);
   const int32_t cxLo = (int32_t)std::floor((x0 - reach) / S), cxHi = (int32_t)std::floor((x1 + reach) / S);
   const int32_t cyLo = (int32_t)std::floor((y0 - reach) / S), cyHi = (int32_t)std::floor((y1 + reach) / S);
   const int32_t bx0 = FloorDiv(cxLo, PACK_BLOCK_CHUNKS), bx1 = FloorDiv(cxHi, PACK_BLOCK_CHUNKS);
   const int32_t by0 = FloorDiv(cyLo, PACK_BLOCK_CHUNKS), by1 = FloorDiv(cyHi, PACK_BLOCK_CHUNKS);
   // Pack the missing blocks in parallel (each is a pure function of its
   // address); PackedBlock below takes them in the usual order. With few
   // missing blocks the relaxation inside each block gets the threads
   // instead (Relax runs its cells in parallel when not nested).
   std::vector<std::pair<int32_t, int32_t>> missing;
   for (int32_t bx = bx0; bx <= bx1; bx++)
      for (int32_t by = by0; by <= by1; by++)
         if (!blocks_.count(std::make_pair(bx, by)) && !(store_ && store_->Find(bx, by)))
            missing.push_back(std::make_pair(bx, by));
   // The block cache's reset (PackedBlock) must not happen in the loop below:
   // the pointers returned would dangle. Reset here if it would.
   if (blocks_.size() + missing.size() >= BLOCK_CACHE_MAX) blocks_.clear();
   if (missing.size() > 1 && (int)missing.size() >= WorldThreads() / 2) {
      std::vector<std::vector<Cell>> built(missing.size());
      ParallelFor(missing.size(), 1, [&](size_t i) { built[i] = PackBlock(missing[i].first, missing[i].second); });
      for (size_t i = 0; i < missing.size(); i++) packPrebuilt_[missing[i]] = std::move(built[i]);
   }
   for (int32_t bx = bx0; bx <= bx1; bx++)
      for (int32_t by = by0; by <= by1; by++)
         for (const Cell& c : PackedBlock(bx, by))
            if (RectDist(c.x, c.y, x0, y0, x1, y1) <= c.rOuter) out.push_back(&c);
   packPrebuilt_.clear();
   if (store_ && store_->Pending() >= 16) store_->Flush();
}

CellAssets& World::Assets(const Cell& c, bool withMts)
{
   PoolScope scope(pool_);
   const auto key = std::make_pair(c.cx, c.cy);
   for (auto it = assets_.begin(); it != assets_.end(); ++it) {
      if (it->first != key) continue;
      if (it != assets_.begin()) assets_.splice(assets_.begin(), assets_, it);
      CellAssets& a = *assets_.front().second;
      if (withMts) EnsureMts(seed_, a, p_);
      return a;
   }
   std::unique_ptr<CellAssets> a;
   auto pre = assetPrebuilt_.find(key);
   if (pre != assetPrebuilt_.end()) {
      a = std::move(pre->second);
      assetPrebuilt_.erase(pre);
   } else {
      a = BuildCellAssets(seed_, c, p_, withMts);
   }
   if (withMts) EnsureMts(seed_, *a, p_);
   stats_.cellsBuilt++;
   assets_.emplace_front(key, std::move(a));
   while (assets_.size() > assetCap_) assets_.pop_back();
   return *assets_.front().second;
}

void World::PrebuildAssets(const std::vector<const Cell*>& cells)
{
   const bool withNpcs = NpcsLabelled();
   std::vector<const Cell*> missing;
   std::vector<CellAssets*> partial;   // cached from a mesh-only request: microtubules (or pores) still to build
   for (const Cell* cp : cells) {
      const Cell& c = *cp;
      const auto key = std::make_pair(c.cx, c.cy);
      bool known = assetPrebuilt_.count(key) > 0;
      CellAssets* have = nullptr;
      for (auto it = assets_.begin(); !known && it != assets_.end(); ++it)
         if (it->first == key) { known = true; have = it->second.get(); }
      if (!known) {
         missing.push_back(cp);
         assetPrebuilt_[key] = nullptr;
      } else if (have && (!have->mtsBuilt || (withNpcs && !have->npcsBuilt))) {
         partial.push_back(have);
      }
   }
   assetPrebuilt_.clear();
   // Few cells: each cell's microtubules run in parallel instead (see CellsInRect).
   const size_t work = missing.size() + partial.size();
   if (work > 1 && (int)work >= WorldThreads() / 2) {
      std::vector<std::unique_ptr<CellAssets>> built(missing.size());
      ParallelFor(work, 1, [&](size_t i) {
         if (i < missing.size()) built[i] = BuildCellAssets(seed_, *missing[i], p_, true, withNpcs);
         else {
            EnsureMts(seed_, *partial[i - missing.size()], p_);
            if (withNpcs) EnsureNpcs(seed_, *partial[i - missing.size()], p_);
         }
      });
      for (size_t i = 0; i < missing.size(); i++)
         assetPrebuilt_[std::make_pair(missing[i]->cx, missing[i]->cy)] = std::move(built[i]);
   }
}

template <class Fn>
void World::ForEachDyeBlock(double x0, double y0, double x1, double y1, double zMin, double zMax,
                            const BlockPrep& prep, const BlockNeeds& needs, Fn fn)
{
   query_++;
   if (origami_) {
      ForEachOrigamiBlock(x0, y0, x1, y1, prep, needs, fn);
      return;
   }
   std::vector<const Cell*> cells;
   CellsInRectPtr(x0, y0, x1, y1, cells);
   // The assets of the cells not cached, built in parallel (Assets takes them
   // in the usual order). Not under a deadline: that walk may stop early.
   if (!deadline_) PrebuildAssets(cells);
   const double mtReach = MtDyeReach(), blockReach = DYE_BLOCK_UM / 2 + mtReach;
   // The pores (JS world.js forEachDyeBlock): blocks reaching within rb of the rect, after the microtubules'.
   const bool npcOn = NpcsLabelled();
   const double npcRb = std::sqrt(3.0) / 2 * NPC_BLOCK_UM + NpcDyeReach();
   struct Item { DyeBlock* b; int mt, block; };   // mt -1: a pore block
   std::vector<Item> batch;
   std::vector<size_t> work;
   for (const Cell* cp : cells) {
      const Cell& c = *cp;
      if (PastDeadline()) break;
      CellAssets& A = Assets(c);
      const double centreDist = RectDist(c.x, c.y, x0, y0, x1, y1);
      batch.clear();
      for (size_t i = 0; i < A.mts.size(); i++) {
         if (A.mts[i].pts.size() < 2 || centreDist > A.mtReach[i] + mtReach) continue;
         // The blocks' midpoints (world x, y and z), kept with the assets.
         const std::vector<CellAssets::BlockMid>& mids = A.Mids(i);
         for (int b = 0; b < (int)mids.size(); b++) {
            const CellAssets::BlockMid& mid = mids[(size_t)b];
            if (mid.z + blockReach < zMin || mid.z - blockReach >= zMax) continue;
            if (RectDist(mid.wx, mid.wy, x0, y0, x1, y1) > blockReach) continue;
            if (PastDeadline()) break;
            batch.push_back({ &FindDyeBlock(c, STRUCTURE_MT, (int)i, b), (int)i, b });
         }
      }
      if (npcOn) {
         EnsureNpcs(seed_, A, p_);
         if (!A.npcBlocks.empty() && !(centreDist > A.npcReachXY + npcRb)) {
            for (int b = 0; b < (int)A.npcBlocks.size(); b++) {
               const CellAssets::NpcBlock& blk = A.npcBlocks[(size_t)b];
               if (blk.z + npcRb < zMin || blk.z - npcRb >= zMax) continue;
               if (RectDist(blk.wx, blk.wy, x0, y0, x1, y1) > npcRb) continue;
               if (PastDeadline()) break;
               batch.push_back({ &FindDyeBlock(c, STRUCTURE_NPC, 0, b), -1, b });
            }
         }
      }
      // This cell's new dye blocks, then the prep work, in parallel; each
      // item touches only its own block.
      work.clear();
      for (size_t k = 0; k < batch.size(); k++)
         if (!batch[k].b->generated) work.push_back(k);
      ParallelFor(work.size(), 2, [&](size_t w) {
         const Item& it = batch[work[w]];
         if (it.mt < 0) GenerateNpcDyes(*it.b, c, A, it.block);
         else GenerateDyes(*it.b, c, A.mts[it.mt].pts, *A.frames[it.mt], it.mt, it.block);
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

template <class Fn>
void World::ForEachOrigamiBlock(double x0, double y0, double x1, double y1, const BlockPrep& prep, const BlockNeeds& needs,
                                Fn fn)
{
   // As the cell walk: the blocks in order (JS forEachDyeBlock: moleculesInRect), the new ones generated and the
   // prep work done in parallel (each item touches only its block), then fn serially.
   std::vector<OrigamiMolecule> mols;
   origami_->MoleculesInRect(x0, y0, x1, y1, mols);
   std::vector<DyeBlock*> batch;
   batch.reserve(mols.size());
   for (const OrigamiMolecule& m : mols) {
      if (PastDeadline()) break;
      const BlockKey key = { m.tx, m.ty, STRUCTURE_ORIGAMI, m.i, 0 };
      auto it = dyeIndex_.find(key);
      if (it != dyeIndex_.end()) {
         stats_.dyeBlockHits++;
         if (it->second != dyeLru_.begin()) dyeLru_.splice(dyeLru_.begin(), dyeLru_, it->second);
         dyeLru_.front().second.used = query_;
         batch.push_back(&dyeLru_.front().second);
         continue;
      }
      DyeBlock blk;
      blk.used = query_;
      stats_.dyeBlocks++;
      dyeLru_.emplace_front(key, std::move(blk));
      dyeIndex_[key] = dyeLru_.begin();
      batch.push_back(&dyeLru_.front().second);
   }
   std::vector<size_t> work;
   for (size_t k = 0; k < batch.size(); k++)
      if (!batch[k]->generated) work.push_back(k);
   ParallelFor(work.size(), 8, [&](size_t w) { GenerateOrigamiDyes(*batch[work[w]], mols[work[w]]); });
   for (size_t k : work) dyeCount_ += batch[k]->dyes.size();
   if (prep) {
      work.clear();
      for (size_t k = 0; k < batch.size(); k++)
         if (needs(*batch[k])) work.push_back(k);
      ParallelFor(work.size(), 2, [&](size_t w) { prep(*batch[work[w]]); });
   }
   for (DyeBlock* b : batch) fn(*b);
   while (dyeCount_ > dyeCap_ && !dyeLru_.empty() && dyeLru_.back().second.used != query_) {
      dyeCount_ -= dyeLru_.back().second.dyes.size();
      dyeIndex_.erase(dyeLru_.back().first);
      dyeLru_.pop_back();
      evictions_++;
   }
}

void World::GenerateOrigamiDyes(DyeBlock& blk, const OrigamiMolecule& m) const
{
   const Label& label = labels_[STRUCTURE_ORIGAMI];
   std::vector<OrigamiSite> sites;
   OrigamiMoleculeSites(origami_->Design(), origami_->P(), m, origami_->Shape(m), label.density,
                        label.fluorescentFraction, sites);
   blk.dyes.clear();
   blk.dyes.reserve(sites.size());
   blk.cx = m.tx; blk.cy = m.ty; blk.mtIndex = m.i;
   blk.h1 = m.h1;
   blk.structure = STRUCTURE_ORIGAMI;
   blk.zLo = INFINITY; blk.zHi = -INFINITY;
   for (const OrigamiSite& d : sites) {
      blk.dyes.push_back({ d.x, d.y, d.z, d.id, ((uint32_t)d.k << 24) | ((uint32_t)d.n & 0xFFFFFFu) });
      blk.zLo = std::min(blk.zLo, d.z);
      blk.zHi = std::max(blk.zHi, d.z);
   }
   blk.phase = Pcg4d((uint32_t)m.tx, (uint32_t)m.ty, (uint32_t)m.i, ORIGAMI_SALT).a;
   blk.generated = true;
}

bool World::PastDeadline()
{
   if (!deadline_) return false;
   if (!stopped_ && std::chrono::steady_clock::now() >= *deadline_) stopped_ = true;
   return stopped_;
}

World::PersistRange World::PersistFor(int s, double t0, double t1, const ClockView& v) const
{
   const Kinetics& kin = labels_[(size_t)s].kin;
   const KineticsHistory* h = v.hist;
   const double maxOn = PersistentMaxOn(kin, h);
   bool any = kin.activationRatePerSec > 0;
   if (h) {
      any = false;
      for (const Kinetics& q : h->kin) any = any || q.activationRatePerSec > 0;
   }
   return { any && t1 > t0, std::max(0L, (long)std::floor((t0 - maxOn) / PERSIST_BIN_SEC)),
            (long)std::floor(t1 / PERSIST_BIN_SEC) };
}

bool World::Prefetch(double x0, double y0, double x1, double y1, double zMin, double zMax, double t0, double t1,
                     double budgetMs)
{
   PoolScope scope(pool_);
   // Nothing new since the last complete prefetch of a region holding this one.
   const PrefetchRegion r = { x0, y0, x1, y1, zMin, zMax, evictions_, labelVersion_, histVersion_, true };
   const PrefetchRegion& d = prefetchDone_;
   if (d.valid && x0 >= d.x0 && y0 >= d.y0 && x1 <= d.x1 && y1 <= d.y1 && zMin >= d.zMin && zMax <= d.zMax &&
       d.evictions == evictions_ && d.labelVersion == labelVersion_ && d.histVersion == histVersion_)
      return true;
   const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double, std::milli>(std::max(0.0, budgetMs)));
   deadline_ = &deadline;
   stopped_ = false;
   PrepCounts n;
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax,
      [&](DyeBlock& b) {
         if (Schedule(b, t0, t1)) n.schedules++;
         const PersistRange pr = PersistFor(b.structure, t0, t1);
         if (pr.persist && b.persistent && PersistentCover(b, pr.b0, pr.b1, t1, false)) n.covers++;
      },
      [&](const DyeBlock& b) {
         if (b.dyes.empty()) return false;
         if (!ScheduleCovers(b, t0, t1)) return true;
         const PersistRange pr = PersistFor(b.structure, t0, t1);
         return pr.persist && b.persistent && CoverWouldBuild(b, pr.b0, pr.b1, t1);
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

World::DyeBlock& World::FindDyeBlock(const Cell& c, int structure, int element, int block)
{
   const BlockKey key = { c.cx, c.cy, structure, element, block };
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
   const Label& label = labels_[STRUCTURE_MT];
   std::vector<Dye> dyes;
   // ~1625 lattice sites per block; the fluorescent fraction of them.
   dyes.reserve((size_t)(1625 * std::min(1.0, std::max(0.0, label.density)) *
                         std::min(1.0, std::max(0.0, label.fluorescentFraction))) + 16);
   DyesInBlock(seed_, c.cx, c.cy, mtIndex, pts, fr, block, label.density, label.fluorescentFraction, dyes, label.linkage);
   blk.dyes.clear();
   blk.dyes.reserve(dyes.size());
   blk.cx = c.cx; blk.cy = c.cy; blk.mtIndex = mtIndex;
   blk.h1 = DyeH1(seed_, c.cx, c.cy, mtIndex);
   blk.structure = STRUCTURE_MT;
   blk.zLo = INFINITY; blk.zHi = -INFINITY;
   // LocalToWorld's cos/sin of packRot once per block, not per dye (its branch kept).
   const double rot = c.packRot;
   const bool rotated = !(rot == 0 || std::isnan(rot));
   const double cr = rotated ? jsm::cos(rot) : 1, sr = rotated ? jsm::sin(rot) : 0;
   for (const Dye& d : dyes) {
      double wx, wy;
      if (!rotated) { wx = c.x + d.pos.x; wy = c.y + d.pos.y; }
      else { wx = c.x + d.pos.x * cr - d.pos.y * sr; wy = c.y + d.pos.x * sr + d.pos.y * cr; }
      blk.dyes.push_back({ wx, wy, d.pos.z, d.id, ((uint32_t)d.k << 24) | ((uint32_t)d.n & 0xFFFFFFu) });
      blk.zLo = std::min(blk.zLo, d.pos.z);
      blk.zHi = std::max(blk.zHi, d.pos.z);
   }
   blk.phase = Pcg4d((uint32_t)c.cx, (uint32_t)c.cy, (uint32_t)mtIndex, (uint32_t)block).a;
   blk.generated = true;
}

void World::GenerateNpcDyes(DyeBlock& blk, const Cell& c, const CellAssets& A, int block) const
{
   const Label& label = labels_[STRUCTURE_NPC];
   std::vector<NpcDye> dyes;
   NpcDyesInBlock(seed_, c.cx, c.cy, A.npcs, A.npcBlocks[(size_t)block].ids, A.npcSites, label.density,
                  label.fluorescentFraction, dyes, label.linkage);
   blk.dyes.clear();
   blk.dyes.reserve(dyes.size());
   blk.cx = c.cx; blk.cy = c.cy; blk.mtIndex = -1;
   blk.h1 = NpcH1(seed_, c.cx, c.cy);
   blk.structure = STRUCTURE_NPC;
   blk.zLo = INFINITY; blk.zHi = -INFINITY;
   // As GenerateDyes: LocalToWorld's cos/sin of packRot once per block (its branch kept).
   const double rot = c.packRot;
   const bool rotated = !(rot == 0 || std::isnan(rot));
   const double cr = rotated ? jsm::cos(rot) : 1, sr = rotated ? jsm::sin(rot) : 0;
   for (const NpcDye& d : dyes) {
      double wx, wy;
      if (!rotated) { wx = c.x + d.pos.x; wy = c.y + d.pos.y; }
      else { wx = c.x + d.pos.x * cr - d.pos.y * sr; wy = c.y + d.pos.x * sr + d.pos.y * cr; }
      blk.dyes.push_back({ wx, wy, d.pos.z, d.id, ((uint32_t)d.k << 24) | ((uint32_t)d.n & 0xFFFFFFu) });
      blk.zLo = std::min(blk.zLo, d.pos.z);
      blk.zHi = std::max(blk.zHi, d.pos.z);
   }
   blk.phase = Pcg4d((uint32_t)c.cx, (uint32_t)c.cy, 0x80000000u | (uint32_t)block, NPC_DYE_SALT).a;
   blk.generated = true;
}

void World::UseHistorySlot(DyeBlock& b, uint64_t fp) const
{
   if (b.fp == fp) return;
   DyeBlock::Slot cur;
   cur.fp = b.fp;
   cur.scheduled = b.scheduled;
   cur.tLo = b.tLo;
   cur.horizon = b.horizon;
   cur.maxOn = b.maxOn;
   cur.events.swap(b.events);
   cur.pEvents.swap(b.pEvents);
   cur.pBin0 = b.pBin0;
   cur.pBin1 = b.pBin1;
   auto it = std::find_if(b.parked.begin(), b.parked.end(), [&](const DyeBlock::Slot& s) { return s.fp == fp; });
   if (it != b.parked.end()) {
      b.scheduled = it->scheduled;
      b.tLo = it->tLo;
      b.horizon = it->horizon;
      b.maxOn = it->maxOn;
      b.events.swap(it->events);
      b.pEvents.swap(it->pEvents);
      b.pBin0 = it->pBin0;
      b.pBin1 = it->pBin1;
      b.parked.erase(it);
   } else {
      b.scheduled = false;
      b.tLo = b.horizon = b.maxOn = 0;
      b.pBin0 = b.pBin1 = 0;
   }
   b.fp = fp;
   if (cur.scheduled || !cur.pEvents.empty()) {
      b.parked.push_back(std::move(cur));
      if (b.parked.size() > kParkedSlots) b.parked.erase(b.parked.begin());   // the oldest
   }
}

bool World::SetKineticsHistory(const std::vector<double>& tStart, const std::vector<Kinetics>& kin)
{
   const size_t n = tStart.size();
   if (kin.size() != n * STRUCTURE_COUNT) return false;
   for (size_t i = 0; i < n; i++)
      if (!std::isfinite(tStart[i]) || (i == 0 ? tStart[i] != 0 : !(tStart[i] > tStart[i - 1]))) return false;
   for (const Kinetics& q : kin)
      if (!std::isfinite(q.activationRatePerSec) || !(q.activationRatePerSec >= 0) || !std::isfinite(q.onSec) ||
          !(q.onSec > 0) || !std::isfinite(q.offSec) || !(q.offSec >= 0) || !std::isfinite(q.bleachProb) ||
          !(q.bleachProb > 0) || !std::isfinite(q.photonCV) || !(q.photonCV >= 0) || !std::isfinite(q.initialOnSec) ||
          !(q.initialOnSec >= 0))
         return false;
   bool changed = false;
   for (int s = 0; s < STRUCTURE_COUNT; s++) {
      KineticsHistory h;
      h.tStart = tStart;
      for (size_t i = 0; i < n; i++) h.kin.push_back(kin[i * STRUCTURE_COUNT + (size_t)s]);
      // FNV-1a over the segments' bits (0 is kept for "no history").
      uint64_t fp = 0;
      if (n) {
         fp = 0xcbf29ce484222325ull;
         auto mix = [&](double v) {
            uint64_t u;
            std::memcpy(&u, &v, sizeof u);
            for (int byte = 0; byte < 8; byte++) { fp ^= (u >> (8 * byte)) & 0xFF; fp *= 0x100000001b3ull; }
         };
         for (size_t i = 0; i < n; i++) {
            const Kinetics& q = h.kin[i];
            mix(h.tStart[i]); mix(q.activationRatePerSec); mix(q.onSec); mix(q.offSec);
            mix(q.bleachProb); mix(q.photonCV); mix(q.initialOnSec);
         }
         if (fp == 0) fp = 1;
      }
      if (fp != histFp_[(size_t)s]) changed = true;
      hist_[(size_t)s] = std::move(h);
      histFp_[(size_t)s] = fp;
   }
   if (changed) {
      // The prefetch's key: the histories themselves (two alternating
      // histories do not redo each other's prefetch).
      histVersion_ = 0;
      for (uint64_t f : histFp_) histVersion_ = (histVersion_ ^ f) * 0x100000001b3ull + 0x9E3779B97F4A7C15ull;
   }
   return true;
}

bool World::Schedule(DyeBlock& b, double tLo, double tMax, const ClockView& v) const
{
   UseHistorySlot(b, v.fp);
   if (b.scheduled && b.horizon >= tMax && b.tLo <= tLo) return false;
   const double horizon = 2 * tMax;
   b.events.clear();
   b.maxOn = 0;
   const Label& label = labels_[(size_t)b.structure];
   b.persistent = label.mode == LabelMode::DnaPaint;
   if (!b.persistent && label.mode != LabelMode::WideField) {
      std::vector<Blink> blinks;
      KineticsHistory own;   // a dose clock with epochs: each dye's history from its bounds (ABI 14)
      for (uint32_t i = 0; i < (uint32_t)b.dyes.size(); i++) {
         const PackedDye& d = b.dyes[i];
         blinks.clear();
         const KineticsHistory* h = v.hist;
         if (v.dose) {
            DoseHistory(*map_, *v.dose, d.x, d.y, d.z, own);
            h = &own;
         }
         LabelSchedule(b.h1, d.K(), d.N(), label, &blinks, nullptr, horizon, h);
         for (uint32_t j = 0; j < (uint32_t)blinks.size(); j++) {
            const Blink& bl = blinks[j];
            if (!(bl.tOff > tLo)) continue;
            WorldEvent e = { d.x, d.y, d.z, bl.tOn, bl.tOff, bl.brightness, 0.0, d.id, (uint8_t)b.structure,
                             STATE_BLINK };
            e.key = (uint64_t)i << 32 | j;
            b.events.push_back(e);
            b.maxOn = std::max(b.maxOn, bl.tOff - bl.tOn);
         }
      }
      // Stable order: by tOn, ties by dye order (deterministic either way).
      std::stable_sort(b.events.begin(), b.events.end(),
                       [](const WorldEvent& a, const WorldEvent& e) { return a.tOn < e.tOn; });
   }
   b.scheduled = true;
   b.horizon = horizon;
   b.tLo = tLo;
   return true;
}

bool World::CoverWouldBuild(const DyeBlock& b, long b0, long b1, double t1, uint64_t fp) const
{
   if (b.fp != fp || b0 < b.pBin0 || b0 >= b.pBin1) return true;
   const double frac = b.phase * (1.0 / 4294967296.0);
   return !(b1 < b.pBin1 && t1 < (b.pBin1 - 1 + frac) * PERSIST_BIN_SEC);
}

bool World::PersistentCover(DyeBlock& b, long b0, long b1, double t1, bool shortFirst, const ClockView& v) const
{
   UseHistorySlot(b, v.fp);
   const Kinetics& kin = labels_[(size_t)b.structure].kin;
   const KineticsHistory* hist = v.hist;
   double rate = kin.activationRatePerSec;
   if (hist) {
      rate = 0;
      for (const Kinetics& q : hist->kin) rate = std::max(rate, q.activationRatePerSec);
   }
   // Bins per build: up to 16 or ~2048 expected blinks.
   const double perBin = (double)b.dyes.size() * rate * PERSIST_BIN_SEC;
   const long L = (long)std::min(16.0, std::max(1.0, std::floor(2048.0 / std::max(perBin, 1e-9))));
   long lo, hi;
   if (b0 < b.pBin0 || b0 >= b.pBin1) {
      // First use, a jump in time or a new label: build from scratch -- for a
      // query (shortFirst) only one bin ahead, so a window full of new blocks
      // (a stage jump, a label change) builds what it shows now and the rest
      // in the staggered extensions below.
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
   for (uint32_t i = 0; i < (uint32_t)b.dyes.size(); i++) {
      const PackedDye& d = b.dyes[i];
      bl.clear();
      PersistentBlinksInBins(b.h1, d.K(), d.N(), kin, lo, hi - 1, bl, hist);
      for (const BinBlink& e : bl) b.pEvents.push_back({ e.tOn, e.tOff, e.brightness, i, e.bin, e.j });
   }
   // The new bins all start after the kept ones, so sorting them keeps the
   // whole list sorted by tOn.
   std::sort(b.pEvents.begin() + start, b.pEvents.end(),
             [](const PersistentEvent& a, const PersistentEvent& e) { return a.tOn < e.tOn; });
   b.pBin1 = hi;
   return true;
}

bool World::SetLabel(int structure, const Label& l)
{
   if (structure < 0 || structure >= STRUCTURE_COUNT || ValidateLabel(l)) return false;
   Label nl = l;
   nl.linkage = NormLinkage(nl.hasLinkage ? nl.linkage : LegacyLinkage(structure));
   nl.hasLinkage = true;
   Label& cur = labels_[(size_t)structure];
   const bool dyesChange = l.density != cur.density || l.fluorescentFraction != cur.fluorescentFraction ||
                           nl.linkage != cur.linkage;
   // Only the kinetics change: the schedules made under a kinetics history
   // (fp != 0) never read the label's kinetics, so they stay.
   const bool kinOnly = !dyesChange && l.mode == cur.mode && l.preState == cur.preState &&
                        l.orientation.mode == cur.orientation.mode && l.orientation.polarDeg == cur.orientation.polarDeg &&
                        l.orientation.azimuthDeg == cur.orientation.azimuthDeg &&
                        l.orientation.wobbleDeg == cur.orientation.wobbleDeg && l.motion == cur.motion &&
                        l.offTargetCount == cur.offTargetCount;
   cur = nl;
   labelVersion_++;
   prefetchDone_.valid = false;
   if (dyesChange) {
      dyeLru_.clear();
      dyeIndex_.clear();
      dyeCount_ = 0;
      return true;
   }
   for (auto& e : dyeLru_) {
      if (e.second.structure != structure) continue;
      if (kinOnly) {
         auto& pk = e.second.parked;
         pk.erase(std::remove_if(pk.begin(), pk.end(), [](const DyeBlock::Slot& x) { return x.fp == 0; }), pk.end());
         if (e.second.fp != 0) continue;
      }
      e.second.scheduled = false;
      e.second.map.reset();
      std::vector<WorldEvent>().swap(e.second.events);
      std::vector<PersistentEvent>().swap(e.second.pEvents);
      e.second.pBin0 = e.second.pBin1 = 0;
      if (!kinOnly) e.second.parked.clear();
   }
   return true;
}

bool World::CellNpcs(int32_t cx, int32_t cy, std::vector<Npc>& out)
{
   PoolScope scope(pool_);
   Cell c;
   if (!FindCell(cx, cy, c)) return false;
   CellAssets& A = Assets(c, false);
   EnsureNpcs(seed_, A, p_);
   out = A.npcs;
   return true;
}

bool World::FindCell(int32_t cx, int32_t cy, Cell& out)
{
   if (origami_) return false;
   PoolScope scope(pool_);
   for (const Cell& c : PackedBlock(FloorDiv(cx, PACK_BLOCK_CHUNKS), FloorDiv(cy, PACK_BLOCK_CHUNKS)))
      if (c.cx == cx && c.cy == cy) { out = c; return true; }
   return false;
}

const std::vector<Cell>& World::BlockCells(int32_t bx, int32_t by)
{
   static const std::vector<Cell> none;
   if (origami_) return none;
   PoolScope scope(pool_);
   return PackedBlock(bx, by);
}

bool World::SetPackedBlock(int32_t bx, int32_t by, const double* rows, int32_t n, int stride, bool& skipped)
{
   skipped = false;
   if (origami_) return false;
   const auto key = std::make_pair(bx, by);
   if (blocks_.count(key)) { skipped = true; return true; }
   std::vector<Cell> cells;
   if (!CellsFromRows(bx, by, rows, n, stride, cells)) return false;
   if (blocks_.size() >= BLOCK_CACHE_MAX) blocks_.clear();
   if (store_) store_->Put(bx, by, StoreRows(cells));
   blocks_.emplace(key, std::move(cells));
   stats_.blocksInjected++;
   return true;
}

bool World::CellsFromRows(int32_t bx, int32_t by, const double* rows, int32_t n, int stride, std::vector<Cell>& cells) const
{
   if (n < 0 || stride < 5 || (n > 0 && !rows)) return false;
   // Relax moves x, y, packRot and Prune drops cells: everything else of a
   // packed cell is RawCandidate's, recomputed here from the address.
   cells.clear();
   cells.reserve((size_t)n);
   int32_t prevCx = 0, prevCy = 0;
   for (int32_t i = 0; i < n; i++) {
      const double* r = rows + (size_t)i * stride;
      const double dcx = r[0], dcy = r[1];
      if (!(std::fabs(dcx) < 1e9) || !(std::fabs(dcy) < 1e9) || dcx != std::floor(dcx) || dcy != std::floor(dcy)) return false;
      const int32_t cx = (int32_t)dcx, cy = (int32_t)dcy;
      if (FloorDiv(cx, PACK_BLOCK_CHUNKS) != bx || FloorDiv(cy, PACK_BLOCK_CHUNKS) != by) return false;
      if (i > 0 && !(cx > prevCx || (cx == prevCx && cy > prevCy))) return false;
      if (!std::isfinite(r[2]) || !std::isfinite(r[3]) || !std::isfinite(r[4])) return false;
      prevCx = cx; prevCy = cy;
      Cell c = RawCandidate(seed_, cx, cy, p_);
      if (!c.present) return false;
      c.x = r[2]; c.y = r[3]; c.packRot = r[4];
      cells.push_back(c);
   }
   return true;
}

template <class Fn>
void World::ForEachDye(double x0, double y0, double x1, double y1, double zMin, double zMax, Fn fn)
{
   ForEachDyeBlock(x0, y0, x1, y1, zMin, zMax, nullptr, nullptr, [&](DyeBlock& b) {
      if (b.dyes.empty() || b.zHi < zMin || b.zLo >= zMax) return;
      for (const PackedDye& d : b.dyes)
         if (d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) fn(b, d);
   });
}

void World::SitesInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                          std::vector<WorldDye>& out)
{
   PoolScope scope(pool_);
   ForEachDye(x0, y0, x1, y1, zMin, zMax, [&](const DyeBlock& b, const PackedDye& d) { out.push_back(DyeAt(b, d)); });
}

void World::EventsInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                           double t0, double t1, std::vector<WorldEvent>& out)
{
   PoolScope scope(pool_);
   // Blocks the window can see: their schedules and persistent blinks are
   // built (in parallel) before the serial pass below reads them.
   auto visible = [&](const DyeBlock& b) { return !b.dyes.empty() && !(b.zHi < zMin || b.zLo >= zMax); };
   PrepCounts n;
   auto prep = [&](DyeBlock& b) {
      if (Schedule(b, t0, t1)) n.schedules++;
      const PersistRange pr = PersistFor(b.structure, t0, t1);
      if (pr.persist && b.persistent && PersistentCover(b, pr.b0, pr.b1, t1, true)) n.covers++;
   };
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax, prep,
      [&](const DyeBlock& b) {
         if (!visible(b)) return false;
         if (!ScheduleCovers(b, t0, t1)) return true;
         const PersistRange pr = PersistFor(b.structure, t0, t1);
         return pr.persist && b.persistent && CoverWouldBuild(b, pr.b0, pr.b1, t1);
      },
      [&](DyeBlock& b) {
         if (!visible(b)) return;
         // Built above; this only checks (a block the prep skipped needs nothing).
         prep(b);
         // Only blinks with tOn in [t0 - maxOn, t1) can overlap [t0, t1).
         auto it = std::lower_bound(b.events.begin(), b.events.end(), t0 - b.maxOn,
                                    [](const WorldEvent& e, double t) { return e.tOn < t; });
         for (; it != b.events.end() && it->tOn < t1; ++it) {
            const WorldEvent& e = *it;
            if (e.tOff > t0 && e.z >= zMin && e.z < zMax && e.x >= x0 && e.x < x1 && e.y >= y0 && e.y < y1)
               out.push_back(e);
         }
         // DNA-PAINT sites never bleach, so their blinks are addressed per time
         // bin (see PersistentBlinks) and cached for a range of bins. The answer
         // is PersistentBlinks' for every site in the window, in its order
         // (site, bin, j).
         const PersistRange pr = PersistFor(b.structure, t0, t1);
         if (!b.persistent || !pr.persist) return;
         std::vector<const PersistentEvent*>& hit = persistentScratch_;
         hit.clear();
         auto p = std::lower_bound(b.pEvents.begin(), b.pEvents.end(), pr.b0 * PERSIST_BIN_SEC,
                                   [](const PersistentEvent& e, double t) { return e.tOn < t; });
         for (; p != b.pEvents.end() && p->tOn < t1; ++p) {
            if (p->bin < (uint32_t)pr.b0 || !(p->tOff > t0)) continue;
            const PackedDye& d = b.dyes[p->dye];
            if (d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) hit.push_back(&*p);
         }
         std::sort(hit.begin(), hit.end(), [](const PersistentEvent* a, const PersistentEvent* e) {
            return a->dye != e->dye ? a->dye < e->dye : a->bin != e->bin ? a->bin < e->bin : a->j < e->j;
         });
         for (const PersistentEvent* e : hit) {
            const PackedDye& d = b.dyes[e->dye];
            out.push_back({ d.x, d.y, d.z, e->tOn, e->tOff, e->brightness, 0.0, d.id, (uint8_t)b.structure, STATE_BLINK });
         }
      });
   stats_.schedulesBuilt += n.schedules.load();
   stats_.persistentBuilt += n.covers.load();
}

void World::ContinuousInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax, double tMin,
                               std::vector<WorldEvent>& out)
{
   PoolScope scope(pool_);
   std::vector<ContWindow> cont;
   ForEachDye(x0, y0, x1, y1, zMin, zMax, [&](const DyeBlock& b, const PackedDye& d) {
      const Label& label = labels_[(size_t)b.structure];
      if (label.mode == LabelMode::DnaPaint) return;
      cont.clear();
      LabelSchedule(b.h1, d.K(), d.N(), label, nullptr, &cont, INFINITY, HistoryOf(b.structure));
      for (const ContWindow& w : cont)
         if (w.tOff > tMin)
            out.push_back({ d.x, d.y, d.z, w.tOn, w.tOff, 1.0, w.aux, d.id, (uint8_t)b.structure, w.state });
   });
}

bool World::DyeOrientationOf(const WorldDye& d, Pt3& dir)
{
   PoolScope scope(pool_);
   const Label& label = labels_[(size_t)d.structure];
   if (label.orientation.mode == OrientationMode::Free) return false;
   // Docking strands: no orientation model (Free only; the JS refuses the others).
   if (d.structure != STRUCTURE_MT && d.structure != STRUCTURE_NPC) return false;
   Cell c;
   if (!FindCell(d.cx, d.cy, c)) return false;
   CellAssets& A = Assets(c);
   if (d.structure == STRUCTURE_NPC) {
      // Fixed: from the pore axis (its outward normal), azimuth from the corner's direction in the ring plane.
      const Orientation& o = label.orientation;
      if (DyeOrientationRandom(NpcH1(seed_, d.cx, d.cy), d.k, d.n, o, dir)) return true;
      EnsureNpcs(seed_, A, p_);
      const std::vector<Npc>& pores = A.npcs;
      if (pores.empty() || d.k < 0 || d.k >= NPC_SITES) return false;
      size_t lo = 0, hi = pores.size() - 1;
      while (lo < hi) {
         const size_t m = (lo + hi) / 2;
         if (pores[m].cand < d.n) lo = m + 1; else hi = m;
      }
      const Npc& q = pores[lo];
      if (q.cand != d.n) return false;
      const double th = ((d.k & 31) >> 2) * jsm::PI / 4, ct = jsm::cos(th), st = jsm::sin(th);
      const Pt3 r = { ct * q.e1.x + st * q.e2.x, ct * q.e1.y + st * q.e2.y, ct * q.e1.z + st * q.e2.z };
      dir = DyeOrientationFixed(o, q.n, r);
      return true;
   }
   if (d.mtIndex < 0 || (size_t)d.mtIndex >= A.mts.size()) return false;
   const MtFrames& fr = A.Frames((size_t)d.mtIndex);
   const double S = (MtProtofilamentOffsetNm(d.k) + d.n * MT_DIMER_NM) * 1e-3;
   const double theta = MtProtofilamentTheta(MtSeamPhase(seed_, d.cx, d.cy, d.mtIndex), d.k);
   return DyeOrientation(DyeH1(seed_, d.cx, d.cy, d.mtIndex), d.k, d.n, label, fr, MtSegmentAt(fr, S), theta, dir);
}

long World::DensityInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                            int nx, int ny, float* out)
{
   // The nz = 1, every-structure case of Density3dInWindow: the same dyes
   // (SitesInWindow's filter), the same bins, integer counts in any order --
   // without materialising the site list.
   return Density3dInWindow(x0, y0, x1, y1, zMin, zMax, nx, ny, 1, (1u << STRUCTURE_COUNT) - 1, out);
}

long World::Density3dInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                              int nx, int ny, int nz, unsigned structureMask, float* out)
{
   PoolScope scope(pool_);
   std::fill(out, out + (size_t)nx * ny * nz, 0.0f);
   if (!structureMask) return 0;
   // Same binning as DensityInWindow, so nz = 1 reproduces it.
   const double sx = nx / (x1 - x0), sy = ny / (y1 - y0);
   const double sz = nz > 1 ? nz / (zMax - zMin) : 0.0;
   long total = 0;
   ForEachDye(x0, y0, x1, y1, zMin, zMax, [&](const DyeBlock& b, const PackedDye& d) {
      if (!((structureMask >> b.structure) & 1u)) return;
      const int ix = std::min(nx - 1, (int)std::floor((d.x - x0) * sx));
      const int iy = std::min(ny - 1, (int)std::floor((d.y - y0) * sy));
      const int iz = nz > 1 ? std::min(nz - 1, (int)std::floor((d.z - zMin) * sz)) : 0;
      out[((size_t)iz * ny + iy) * nx + ix] += 1;
      total++;
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
   PoolScope scope(pool_);
   const size_t plane = (size_t)nx * ny, chan = plane * nz;
   std::fill(out, out + chan * 3, 0.0f);
   std::vector<const Cell*> cells;
   CellsInRectPtr(x0, y0, x1, y1, cells);
   PrebuildAssets(cells);
   const double px = (x1 - x0) / nx, py = (y1 - y0) / ny, dz = (zMax - zMin) / nz;
   const double subW = 1.0 / ((double)sub * sub);
   const double mtArea = jsm::PI * (MT_RADIUS_NM / 1000) * (MT_RADIUS_NM / 1000);
   const double mtStep = 0.5 * std::min(px, std::min(py, dz));
   for (const Cell* cp : cells) {
      const Cell& c = *cp;
      CellAssets& A = Assets(c);
      const CytoMesh& mesh = A.geom.mesh;
      const bool rotated = !(c.packRot == 0 || std::isnan(c.packRot));
      const double cr = rotated ? jsm::cos(c.packRot) : 1, sr = rotated ? jsm::sin(c.packRot) : 0;
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
                     // Nucleus chord through this column (the shaped nucleus), clipped to the body.
                     double below = 0, above = 0, zn0 = 0, zn1 = 0;
                     if (NucleusColumnLocal(c, lx, ly, below, above) && (below > 0 || above > 0)) {
                        zn0 = std::max(0.0, c.nucZ - below);
                        zn1 = std::min(h, c.nucZ + above);
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

// ---- ABI 14: the clock map (clockmap.h; spec/ALGORITHM.md "Exact light history") ----

// A block's map state: made on first use, touched only by its block's prep (in parallel over blocks).
struct World::DyeBlock::MapState {
   // The present (the structure clock's fpNow): DOSE: per dye its dose at tOpen and per second after (valid where
   // mark == gen, made on demand); the bounds over the block of both (ACTIVATION: of the rate).
   uint64_t fpNow = 0;
   uint32_t gen = 0;
   std::vector<uint32_t> mark;
   std::vector<double> clock, rate;
   bool bounded = false;
   double cLo = 0, cHi = 0, rLo = 0, rHi = 0;
   // ACTIVATION: per dye its draw E and its walk over the steps (from: the steps summed, H: the hazard after them;
   // from = CROSSED: activated within them, at H), valid for the steps of fingerprint chain (the first chainLen; a
   // longer list with that prefix continues the walks); each step's increment bound over the block (stepHi, for the
   // steps up to fingerprint stepFp) and their suffix sums remHi. For the present (actFp = fpNow): tAct, the exact
   // activation where known (NaN: not yet; CROSSED: H), the end of its last blink (endAt, NaN: not known), and a
   // min-heap of the dyes not live (a lower bound of their activation, or it), popped as the queries reach it.
   static constexpr uint32_t CROSSED = 0xFFFFFFFFu;
   bool haveE = false;
   std::vector<double> E, H;
   std::vector<uint32_t> from;
   uint64_t chain = 0;
   size_t chainLen = 0;
   uint64_t stepFp = 0;
   std::vector<double> stepHi, remHi;
   bool haveAct = false;
   uint64_t actFp = 0;
   std::vector<double> tAct, endAt;
   std::vector<uint8_t> isLive;
   std::vector<std::pair<double, uint32_t>> heap;
   // The live dyes of EventsInMap: activated before the last query's end, each with its whole schedule in arena (the
   // blinks before next ended by liveT0; the dye dropped once all have). Time going back makes them anew.
   struct Live {
      uint32_t dye, begin, count, next;
   };
   double liveT0 = -INFINITY;
   std::vector<Live> live;
   std::vector<Blink> arena;
   // The continuous windows of a dose or global clock on the dye's own clock (made once per schedule fingerprint
   // contFp), sorted by their end (ties: dye order), each with its place in dye order.
   struct Cont {
      double on, off, aux;
      uint32_t dye, order;
      uint8_t state;
   };
   bool haveCont = false;
   uint64_t contFp = 0;
   std::vector<Cont> cont;
   // The query's answer for this block (made in the parallel prep, taken in block order), and its counts (schedules
   // built, dye clocks read, activation walks), summed into the stats by the serial pass.
   std::vector<WorldEvent> out;
   std::vector<MapWindow> contOut;
   long nSchedules = 0, nDyes = 0, nWalks = 0;
};

namespace {
struct MapQuery {
   double x0, y0, x1, y1, zMin, zMax, t0, t1;
   bool In(double x, double y, double z) const { return z >= zMin && z < zMax && x >= x0 && x < x1 && y >= y0 && y < y1; }
};
} // namespace

bool World::SetClockMap(ClockMap m, std::string* err)
{
   if (m.s.size() != (size_t)STRUCTURE_COUNT) {
      if (err) *err = "the clock map needs one clock per structure";
      return false;
   }
   if (const char* e = m.Finish()) {
      if (err) *err = e;
      return false;
   }
   map_ = std::make_shared<const ClockMap>(std::move(m));
   mapVersion_++;
   return true;
}

int World::MapMode(int s) const
{
   const StructClock& c = map_->s[(size_t)s];
   const Label& label = labels_[(size_t)s];
   if (label.mode == LabelMode::DnaPaint) return CLOCK_GLOBAL;
   if (c.mode == CLOCK_ACTIVATION && label.mode != LabelMode::PALM) return CLOCK_GLOBAL;
   return c.mode;
}

World::ClockView World::MapView(int s) const
{
   const StructClock& c = map_->s[(size_t)s];
   const bool doseEpochs = MapMode(s) == CLOCK_DOSE && c.hist.Size() > 1;
   // A clock read on T where the map gives a dose or activation clock (MapMode): its kinetics as a history in T.
   if (MapMode(s) == CLOCK_GLOBAL && c.mode != CLOCK_GLOBAL) return { &c.hist, c.fpHistT, nullptr };
   return { &c.hist, c.fpSchedule, doseEpochs ? &c : nullptr };
}

void World::BlockBox(DyeBlock& b) const
{
   if (b.box) return;
   double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
   for (const PackedDye& d : b.dyes) {
      x0 = std::min(x0, d.x); x1 = std::max(x1, d.x);
      y0 = std::min(y0, d.y); y1 = std::max(y1, d.y);
   }
   b.bx0 = x0; b.bx1 = x1; b.by0 = y0; b.by1 = y1;
   b.box = true;
}

World::DyeBlock::MapState& World::MapStateOf(DyeBlock& b) const
{
   if (!b.map) b.map = std::make_shared<DyeBlock::MapState>();
   DyeBlock::MapState& ms = *b.map;
   const StructClock& c = map_->s[(size_t)b.structure];
   if (ms.fpNow != c.fpNow || !ms.bounded) {
      ms.fpNow = c.fpNow;
      if (++ms.gen == 0) {
         std::fill(ms.mark.begin(), ms.mark.end(), 0u);
         ms.gen = 1;
      }
      BlockBox(b);
      map_->Bounds(c.total, b.bx0, b.bx1, b.by0, b.by1, b.zLo, b.zHi, c.totalC, ms.cLo, ms.cHi);
      map_->Bounds(c.rate, b.bx0, b.bx1, b.by0, b.by1, b.zLo, b.zHi, c.rateC, ms.rLo, ms.rHi);
      ms.bounded = true;
   }
   return ms;
}

void World::DoseOf(DyeBlock& b, uint32_t i, double& clock, double& rate) const
{
   DyeBlock::MapState& ms = *b.map;
   if (ms.mark.size() != b.dyes.size()) {   // made on the first dose read
      ms.mark.assign(b.dyes.size(), 0u);
      ms.clock.assign(b.dyes.size(), 0.0);
      ms.rate.assign(b.dyes.size(), 0.0);
   }
   if (ms.mark[i] != ms.gen) {
      const StructClock& c = map_->s[(size_t)b.structure];
      const PackedDye& d = b.dyes[i];
      ms.clock[i] = map_->Eval(c.total, d.x, d.y, d.z, c.totalC);
      ms.rate[i] = map_->Eval(c.rate, d.x, d.y, d.z, c.rateC);
      ms.mark[i] = ms.gen;
      ms.nDyes++;
   }
   clock = ms.clock[i];
   rate = ms.rate[i];
}

namespace {
using HeapItem = std::pair<double, uint32_t>;
// A min-heap on (time, dye).
bool HeapAfter(const HeapItem& a, const HeapItem& e) { return a > e; }
} // namespace

void World::ActPrep(DyeBlock& b) const
{
   DyeBlock::MapState& ms = MapStateOf(b);
   const StructClock& c = map_->s[(size_t)b.structure];
   const size_t n = b.dyes.size();
   const double nan = std::numeric_limits<double>::quiet_NaN();
   if (!ms.haveE || ms.E.size() != n) {
      ms.E.resize(n);
      for (size_t i = 0; i < n; i++) ms.E[i] = ActivationDraw(b.h1, b.dyes[i].K(), b.dyes[i].N());
      ms.haveE = true;
      ms.from.clear();
   }
   if (ms.from.size() != n || ms.chainLen > c.steps.size() ||
       (ms.chainLen > 0 && c.stepFp[ms.chainLen - 1] != ms.chain)) {
      // Steps other than those walked: every walk from the start.
      ms.from.assign(n, 0u);
      ms.H.assign(n, 0.0);
      ms.tAct.assign(n, nan);
      ms.endAt.assign(n, nan);
      ms.isLive.assign(n, 0);
      ms.live.clear();
      ms.arena.clear();
      ms.chainLen = 0;
      ms.chain = 0;
      ms.haveAct = false;
   }
   const uint64_t sfp = c.steps.empty() ? 0 : c.stepFp.back();
   if (ms.stepFp != sfp || ms.stepHi.size() != c.steps.size() || ms.remHi.size() != c.steps.size() + 1) {
      // The bounds of the steps both lists share (the same prefix), then the new ones.
      size_t keep = ms.stepHi.size();
      if (!(keep <= c.steps.size() && (keep == 0 || c.stepFp[keep - 1] == ms.stepFp))) keep = 0;
      BlockBox(b);
      ms.stepHi.resize(keep);
      for (size_t k = keep; k < c.steps.size(); k++) {
         double lo, hi;
         map_->Bounds(c.steps[k].field, b.bx0, b.bx1, b.by0, b.by1, b.zLo, b.zHi, c.steps[k].c, lo, hi);
         ms.stepHi.push_back(std::max(0.0, hi));
      }
      ms.remHi.assign(c.steps.size() + 1, 0.0);
      for (size_t k = c.steps.size(); k-- > 0;) ms.remHi[k] = ms.remHi[k + 1] + ms.stepHi[k];
      ms.stepFp = sfp;
   }
   if (ms.haveAct && ms.actFp == c.fpNow) return;
   // A new present: the activations after the steps are new (the live dyes activated in them stay).
   for (size_t i = 0; i < n; i++) {
      if (ms.from[i] == DyeBlock::MapState::CROSSED) {
         ms.tAct[i] = ms.H[i];
      } else {
         ms.tAct[i] = nan;
         ms.endAt[i] = nan;
      }
   }
   size_t k = 0;
   for (const DyeBlock::MapState::Live& L : ms.live) {
      if (ms.from[L.dye] == DyeBlock::MapState::CROSSED) ms.live[k++] = L;
      else ms.isLive[L.dye] = 0;
   }
   ms.live.resize(k);
   ActHeap(b, ms.liveT0);
   ms.haveAct = true;
   ms.actFp = c.fpNow;
}

void World::ActHeap(DyeBlock& b, double tNow) const
{
   DyeBlock::MapState& ms = *b.map;
   const StructClock& c = map_->s[(size_t)b.structure];
   ms.heap.clear();
   for (uint32_t i = 0; i < (uint32_t)b.dyes.size(); i++) {
      if (ms.isLive[i] || ms.endAt[i] <= tNow) continue;   // live, or every blink ended by now
      double key = ms.tAct[i];
      if (std::isnan(key)) {
         // A lower bound: the hazard still missing after every remaining step's most, at the block's highest rate.
         const size_t f = ms.from[i];
         const double g = ms.E[i] - ms.H[i] - ms.remHi[f];
         if (g <= 0) key = f < c.steps.size() ? c.steps[f].t0 : c.tOpen;
         else key = ms.rHi > 0 ? c.tOpen + g / ms.rHi : INFINITY;
      }
      if (key < INFINITY) ms.heap.push_back({ key, i });
   }
   std::make_heap(ms.heap.begin(), ms.heap.end(), HeapAfter);
}

double World::ExactAct(DyeBlock& b, uint32_t i) const
{
   DyeBlock::MapState& ms = *b.map;
   if (!std::isnan(ms.tAct[i])) return ms.tAct[i];
   const StructClock& c = map_->s[(size_t)b.structure];
   const PackedDye& d = b.dyes[i];
   size_t f = ms.from[i];
   double H = ms.H[i];
   if (f < c.steps.size()) {
      const double t = ActivationInSteps(*map_, c, d.x, d.y, d.z, ms.E[i], f, H);
      ms.nWalks++;
      const size_t used = std::isnan(t) ? f : f + 1;
      if (used > ms.chainLen) {
         ms.chainLen = used;
         ms.chain = c.stepFp[used - 1];
      }
      if (!std::isnan(t)) {
         ms.from[i] = DyeBlock::MapState::CROSSED;
         ms.H[i] = t;
         return ms.tAct[i] = t;
      }
      ms.from[i] = (uint32_t)f;
      ms.H[i] = H;
   }
   // After the steps: the open record's rate.
   const double S = map_->Eval(c.rate, d.x, d.y, d.z, c.rateC);
   ms.nDyes++;
   return ms.tAct[i] = S > 0 ? c.tOpen + (ms.E[i] - H) / S : INFINITY;
}

bool World::MapNeeds(const DyeBlock& b, double, double) const
{
   return !b.dyes.empty();
}

bool World::MapPrep(DyeBlock& b, double t0, double t1, bool shortFirst) const
{
   if (b.dyes.empty()) return false;
   const int s = b.structure;
   const int mode = MapMode(s);
   const StructClock& c = map_->s[(size_t)s];
   bool built = false;
   if (mode == CLOCK_GLOBAL) {
      const ClockView v = MapView(s);
      built = Schedule(b, t0, t1, v);
      const PersistRange pr = PersistFor(s, t0, t1, v);
      if (pr.persist && b.persistent && PersistentCover(b, pr.b0, pr.b1, t1, shortFirst, v)) built = true;
      return built;
   }
   DyeBlock::MapState& ms = MapStateOf(b);
   if (mode == CLOCK_DOSE) {
      if (!(ms.rHi > 0)) return false;   // frozen everywhere: nothing to show
      const double d0 = t0 - c.tOpen, d1 = t1 - c.tOpen;
      const double lo = ms.cLo + (d0 >= 0 ? ms.rLo * d0 : ms.rHi * d0);
      const double hi = ms.cHi + (d1 >= 0 ? ms.rHi * d1 : ms.rLo * d1);
      // From a whole second of dose below (the bounds' rounding margin moves with them: a pose change that leaves a
      // block's least dose alone must not rebuild its schedule).
      return Schedule(b, std::max(0.0, std::floor(lo) - 1.0), hi, MapView(s));
   }
   // ACTIVATION: every dye's activation for the present (no schedule: the blinks are made per live dye).
   ActPrep(b);
   return false;
}

void World::EventsInMap(double x0, double y0, double x1, double y1, double zMin, double zMax, double t0, double t1,
                        std::vector<WorldEvent>& out)
{
   if (!map_) {
      EventsInWindow(x0, y0, x1, y1, zMin, zMax, t0, t1, out);
      return;
   }
   PoolScope scope(pool_);
   const MapQuery q = { x0, y0, x1, y1, zMin, zMax, t0, t1 };
   auto visible = [&](const DyeBlock& b) { return !b.dyes.empty() && !(b.zHi < zMin || b.zLo >= zMax); };
   // Each block's answer is made in the parallel prep (it reads and builds only its own caches), then taken in order.
   auto emit = [&](DyeBlock& b) {
      const bool built = MapPrep(b, t0, t1, true);
      MapStateOf(b).nSchedules += built;
      std::vector<WorldEvent> res;
      const int s = b.structure;
      const int mode = MapMode(s);
      const StructClock& c = map_->s[(size_t)s];
      if (mode == CLOCK_GLOBAL) {
         // As EventsInWindow, on T.
         const ClockView v = MapView(s);
         auto it = std::lower_bound(b.events.begin(), b.events.end(), t0 - b.maxOn,
                                    [](const WorldEvent& e, double t) { return e.tOn < t; });
         for (; it != b.events.end() && it->tOn < t1; ++it)
            if (it->tOff > t0 && q.In(it->x, it->y, it->z)) res.push_back(*it);
         const PersistRange pr = PersistFor(s, t0, t1, v);
         if (b.persistent && pr.persist) {
            std::vector<const PersistentEvent*> hit;
            auto p = std::lower_bound(b.pEvents.begin(), b.pEvents.end(), pr.b0 * PERSIST_BIN_SEC,
                                      [](const PersistentEvent& e, double t) { return e.tOn < t; });
            for (; p != b.pEvents.end() && p->tOn < t1; ++p) {
               if (p->bin < (uint32_t)pr.b0 || !(p->tOff > t0)) continue;
               const PackedDye& d = b.dyes[p->dye];
               if (q.In(d.x, d.y, d.z)) hit.push_back(&*p);
            }
            std::sort(hit.begin(), hit.end(), [](const PersistentEvent* a, const PersistentEvent* e) {
               return a->dye != e->dye ? a->dye < e->dye : a->bin != e->bin ? a->bin < e->bin : a->j < e->j;
            });
            for (const PersistentEvent* e : hit) {
               const PackedDye& d = b.dyes[e->dye];
               res.push_back({ d.x, d.y, d.z, e->tOn, e->tOff, e->brightness, 0.0, d.id, (uint8_t)s, STATE_BLINK });
            }
         }
      } else {
         DyeBlock::MapState& ms = MapStateOf(b);
         struct Cand { double tOn, own; uint64_t key; WorldEvent e; };
         std::vector<Cand> cand;
         if (mode == CLOCK_DOSE && ms.rHi > 0) {
            // The block's events on the dose clock that some dye's window can reach, each against its own window,
            // mapped to T at its dye's rate now.
            const double d0 = t0 - c.tOpen, d1 = t1 - c.tOpen;
            const double lo = ms.cLo + (d0 >= 0 ? ms.rLo * d0 : ms.rHi * d0);
            const double hi = ms.cHi + (d1 >= 0 ? ms.rHi * d1 : ms.rLo * d1);
            auto it = std::lower_bound(b.events.begin(), b.events.end(), lo - b.maxOn,
                                       [](const WorldEvent& e, double t) { return e.tOn < t; });
            for (; it != b.events.end() && it->tOn < hi; ++it) {
               const WorldEvent& e = *it;
               if (!(e.tOff > lo) || !q.In(e.x, e.y, e.z)) continue;
               double tau, R;
               DoseOf(b, (uint32_t)(e.key >> 32), tau, R);
               if (!(R > 0)) continue;
               const double a0 = tau + R * d0, a1 = tau + R * d1;
               if (!(e.tOn < a1 && e.tOff > a0)) continue;
               WorldEvent o = e;
               o.tOn = c.tOpen + (e.tOn - tau) / R;
               o.tOff = c.tOpen + (e.tOff - tau) / R;
               cand.push_back({ o.tOn, e.tOn, e.key, o });
            }
         } else if (mode == CLOCK_ACTIVATION) {
            // The live dyes: those activated before t1 whose blinks have not all ended by t0, taken from the heap.
            using Live = DyeBlock::MapState::Live;
            if (t0 < ms.liveT0) {   // time went back: anew from t0
               for (const DyeBlock::MapState::Live& L : ms.live) ms.isLive[L.dye] = 0;
               ms.live.clear();
               ms.arena.clear();
               ActHeap(b, t0);
            }
            ms.liveT0 = t0;
            std::vector<Blink> bl;
            const Label& label = labels_[(size_t)s];
            while (!ms.heap.empty() && ms.heap.front().first < t1) {
               std::pop_heap(ms.heap.begin(), ms.heap.end(), HeapAfter);
               const uint32_t i = ms.heap.back().second;
               ms.heap.pop_back();
               const double tA = ExactAct(b, i);
               if (!(tA < t1)) {   // later: back with its exact time
                  if (tA < INFINITY) {
                     ms.heap.push_back({ tA, i });
                     std::push_heap(ms.heap.begin(), ms.heap.end(), HeapAfter);
                  }
                  continue;
               }
               if (ms.endAt[i] <= t0) continue;   // every blink ended before (NaN: not known yet)
               const PackedDye& d = b.dyes[i];
               bl.clear();
               LabelScheduleActivated(b.h1, d.K(), d.N(), label, &c.hist, tA, &bl, nullptr);
               ms.endAt[i] = bl.empty() ? tA : bl.back().tOff;
               if (ms.endAt[i] <= t0) continue;
               ms.isLive[i] = 1;
               ms.live.push_back({ i, (uint32_t)ms.arena.size(), (uint32_t)bl.size(), 0u });
               ms.arena.insert(ms.arena.end(), bl.begin(), bl.end());
            }
            size_t kept = 0;
            for (size_t k = 0; k < ms.live.size();) {
               Live& L = ms.live[k];
               const Blink* a = ms.arena.data() + L.begin;
               while (L.next < L.count && !(a[L.next].tOff > t0)) L.next++;
               if (L.next == L.count) {   // all ended: never again (time goes on)
                  ms.isLive[L.dye] = 0;
                  ms.live[k] = ms.live.back();
                  ms.live.pop_back();
                  continue;
               }
               kept += L.count;
               const PackedDye& d = b.dyes[L.dye];
               if (q.In(d.x, d.y, d.z))
                  for (uint32_t j = L.next; j < L.count && a[j].tOn < t1; j++) {
                     WorldEvent e = { d.x, d.y, d.z, a[j].tOn, a[j].tOff, a[j].brightness, 0.0, d.id, (uint8_t)s,
                                      STATE_BLINK };
                     e.key = (uint64_t)L.dye << 32 | j;
                     cand.push_back({ e.tOn, e.tOn, e.key, e });
                  }
               k++;
            }
            if (ms.arena.size() > 4096 && ms.arena.size() > 4 * kept) {   // drop the dead dyes' blinks
               std::vector<Blink> a;
               a.reserve(kept);
               for (Live& L : ms.live) {
                  const uint32_t at = (uint32_t)a.size();
                  a.insert(a.end(), ms.arena.begin() + L.begin, ms.arena.begin() + L.begin + L.count);
                  L.begin = at;
               }
               ms.arena.swap(a);
            }
         }
         std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& e) {
            return a.tOn != e.tOn ? a.tOn < e.tOn : a.own != e.own ? a.own < e.own : a.key < e.key;
         });
         res.reserve(cand.size());
         for (const Cand& k : cand) res.push_back(k.e);
      }
      MapStateOf(b).out.swap(res);
   };
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax, emit, [&](const DyeBlock& b) { return visible(b); },
      [&](DyeBlock& b) {
         if (!visible(b) || !b.map) return;
         DyeBlock::MapState& ms = *b.map;
         out.insert(out.end(), ms.out.begin(), ms.out.end());
         ms.out.clear();
         stats_.schedulesBuilt += ms.nSchedules;
         stats_.mapDyes += ms.nDyes;
         stats_.mapWalks += ms.nWalks;
         ms.nSchedules = ms.nDyes = ms.nWalks = 0;
      });
}

void World::ContinuousInMap(double x0, double y0, double x1, double y1, double zMin, double zMax, double tMin,
                            std::vector<MapWindow>& out)
{
   if (!map_) {
      std::vector<WorldEvent> ev;
      ContinuousInWindow(x0, y0, x1, y1, zMin, zMax, tMin, ev);
      for (const WorldEvent& e : ev) out.push_back({ e, 0.0, 1.0 });
      return;
   }
   PoolScope scope(pool_);
   auto visible = [&](const DyeBlock& b) {
      return !b.dyes.empty() && !(b.zHi < zMin || b.zLo >= zMax) &&
             labels_[(size_t)b.structure].mode != LabelMode::DnaPaint;
   };
   auto in = [&](const PackedDye& d) { return d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1; };
   auto emit = [&](DyeBlock& b) {
      const int s = b.structure;
      const Label& label = labels_[(size_t)s];
      const int mode = MapMode(s);
      const StructClock& c = map_->s[(size_t)s];
      DyeBlock::MapState& ms = MapStateOf(b);
      std::vector<MapWindow>& res = ms.contOut;
      res.clear();
      std::vector<ContWindow> cont;
      if (mode == CLOCK_ACTIVATION) {
         // The pre windows end at each dye's activation (made per query from the walk's memo).
         ActPrep(b);
         for (uint32_t i = 0; i < (uint32_t)b.dyes.size(); i++) {
            const PackedDye& d = b.dyes[i];
            if (!in(d)) continue;
            cont.clear();
            const double tA = ExactAct(b, i);
            LabelScheduleActivated(b.h1, d.K(), d.N(), label, &c.hist, tA, nullptr, &cont);
            for (const ContWindow& w : cont)
               if (w.tOff > tMin)
                  res.push_back({ { d.x, d.y, d.z, w.tOn, w.tOff, 1.0, w.aux, d.id, (uint8_t)s, w.state }, tA, 1.0 });
         }
         return;
      }
      // Dose and global clocks: the windows on the dye's own clock, cached per schedule fingerprint.
      const ClockView v = MapView(s);
      if (!ms.haveCont || ms.contFp != v.fp) {
         ms.cont.clear();
         KineticsHistory own;
         uint32_t order = 0;
         for (uint32_t i = 0; i < (uint32_t)b.dyes.size(); i++) {
            const PackedDye& d = b.dyes[i];
            const KineticsHistory* h = v.hist;
            if (v.dose) {
               DoseHistory(*map_, *v.dose, d.x, d.y, d.z, own);
               h = &own;
            }
            cont.clear();
            LabelSchedule(b.h1, d.K(), d.N(), label, nullptr, &cont, INFINITY, h);
            for (const ContWindow& w : cont) ms.cont.push_back({ w.tOn, w.tOff, w.aux, i, order++, w.state });
         }
         std::stable_sort(ms.cont.begin(), ms.cont.end(),
                          [](const DyeBlock::MapState::Cont& a, const DyeBlock::MapState::Cont& e) { return a.off < e.off; });
         ms.haveCont = true;
         ms.contFp = v.fp;
      }
      // Only windows ending after the block's least clock at tMin can still be open.
      double lo = tMin;
      if (mode == CLOCK_DOSE) {
         const double d0 = tMin - c.tOpen;
         lo = ms.cLo + (d0 >= 0 ? ms.rLo * d0 : ms.rHi * d0);
      }
      auto it = std::upper_bound(ms.cont.begin(), ms.cont.end(), lo,
                                 [](double t, const DyeBlock::MapState::Cont& w) { return t < w.off; });
      std::vector<std::pair<uint32_t, MapWindow>> hit;
      for (; it != ms.cont.end(); ++it) {
         const PackedDye& d = b.dyes[it->dye];
         if (!in(d)) continue;
         double on = it->on, off = it->off, clock = c.tOpen, rate = 1.0;
         if (mode == CLOCK_DOSE) {
            DoseOf(b, it->dye, clock, rate);
            if (rate > 0) {
               on = c.tOpen + (it->on - clock) / rate;
               off = c.tOpen + (it->off - clock) / rate;
            } else {   // frozen: a window holding its dose lasts, the others never are
               if (it->on > clock || !(it->off > clock)) continue;
               on = -INFINITY;
               off = INFINITY;
            }
         }
         if (off > tMin)
            hit.push_back({ it->order, { { d.x, d.y, d.z, on, off, 1.0, it->aux, d.id, (uint8_t)s, it->state }, clock, rate } });
      }
      std::sort(hit.begin(), hit.end(), [](const std::pair<uint32_t, MapWindow>& a, const std::pair<uint32_t, MapWindow>& e) {
         return a.first < e.first;
      });
      res.reserve(hit.size());
      for (const auto& h : hit) res.push_back(h.second);
   };
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax, emit, [&](const DyeBlock& b) { return visible(b); },
      [&](DyeBlock& b) {
         if (!visible(b) || !b.map) return;
         DyeBlock::MapState& ms = *b.map;
         out.insert(out.end(), ms.contOut.begin(), ms.contOut.end());
         std::vector<MapWindow>().swap(ms.contOut);
         stats_.mapDyes += ms.nDyes;
         stats_.mapWalks += ms.nWalks;
         ms.nDyes = ms.nWalks = 0;
      });
}

bool World::PrefetchMap(double x0, double y0, double x1, double y1, double zMin, double zMax, double t0, double t1,
                        double budgetMs)
{
   if (!map_) return Prefetch(x0, y0, x1, y1, zMin, zMax, t0, t1, budgetMs);
   PoolScope scope(pool_);
   const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double, std::milli>(std::max(0.0, budgetMs)));
   deadline_ = &deadline;
   stopped_ = false;
   ForEachDyeBlock(
      x0, y0, x1, y1, zMin, zMax, [&](DyeBlock& b) { MapPrep(b, t0, t1, false); },
      [&](const DyeBlock& b) { return MapNeeds(b, t0, t1); }, [](DyeBlock&) {});
   deadline_ = nullptr;
   const bool complete = !stopped_;
   stopped_ = false;
   stats_.prefetches++;
   return complete;
}

} // namespace isc
