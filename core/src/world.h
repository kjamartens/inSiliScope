// The queryable world: cells packed on fixed blocks, their cytoplasm mesh
// and microtubules, and the labelled dyes on those, by world rectangle.
//
// Fixed-block packing (spec/PORT.md 4.3): the chunk grid is cut into
// absolutely aligned PACK_BLOCK_CHUNKS^2 blocks. A block is packed on its own
// candidates plus a margin of InteractionChunks(p) + 2 chunks and keeps only
// the cells whose home chunk lies in the block core, so a cell depends only
// on its block's address, never on the viewport or query order. Cells near a
// block border can differ slightly from what the neighbouring block would
// have decided for the same pair; that is accepted (deterministic).
// enablePacking = false uses the raw jittered candidates (some overlaps).
//
// Every cache here is for speed only: dropping it (DropCaches) gives the
// same answers.
#pragma once

#include "cells.h"
#include "dyes.h"
#include "microtubules.h"
#include "parallel.h"
#include "params.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace isc {

constexpr int PACK_BLOCK_CHUNKS = 8;

struct CellAssets {
   Cell cell;                                // shape + packed pose
   MtCellGeom geom;                          // direction table, area, cytoplasm mesh
   std::vector<Microtubule> mts;
   std::vector<double> mtReach;              // per MT: max distance of a point from the cell centre (um)
   std::vector<std::unique_ptr<MtFrames>> frames;   // built lazily, per MT
   const MtFrames& Frames(size_t i);
   // Per MT, built lazily with Frames(i): the world x, y and the z of every
   // 1 um dye block's arc midpoint -- the values the block walk of a query
   // used to compute again for every block of every query.
   struct BlockMid { double wx, wy, z; };
   std::vector<std::vector<BlockMid>> mids;
   const std::vector<BlockMid>& Mids(size_t i);
};

struct WorldDye {
   double x, y, z;                           // world um (z above the coverslip)
   uint32_t id;                              // per-dye hash (stable across windows)
   int32_t cx, cy, mtIndex;
   int32_t k, n;                             // lattice address on the microtubule
   bool persistent;                          // non-bleaching site (Params::labelNonBleaching)
};

// One blink of one dye, in world um and simulated seconds.
struct WorldEvent {
   double x, y, z;
   double tOn, tOff, brightness;
   uint32_t id;
};

// A persistent-site blink cached in its dye block: dye index in the block,
// and (bin, j) to restore PersistentBlinks' order.
struct PersistentEvent {
   double tOn, tOff, brightness;
   uint32_t dye, bin, j;
};

struct WorldStats {
   long blocksPacked = 0, cellsBuilt = 0, framesBuilt = 0;
   long dyeBlocks = 0;          // 1 um dye blocks generated (cache misses)
   long dyeBlockHits = 0;       // served from the cache
   long schedulesBuilt = 0;     // blocks whose blink schedules were built
   long persistentBuilt = 0;    // persistent-blink bin ranges built
   long prefetches = 0;         // Prefetch passes that scanned
};

class World {
public:
   // Caches: cell assets (LRU, cells) and dye blocks (LRU, bounded by the
   // number of dyes they hold; a 12.8 um FOV beside a nucleus is ~400k dyes
   // at the default 10% labelling, ~50 bytes each plus ~60 per blink). The
   // dye cap is soft: blocks the current query uses are never evicted, so a
   // window with more dyes than the cap is not regenerated on every query.
   World(uint32_t seed, const Params& p, size_t assetCacheCells = 48, size_t dyeCacheDyes = 2000000);

   const Params& GetParams() const { return p_; }
   // Dye cache cap (dyes); a smaller one takes effect at the next query.
   void SetDyeCacheCap(size_t dyes) { dyeCap_ = dyes; }
   uint32_t Seed() const { return seed_; }

   // Cells (packed pose) whose footprint circle (rOuter) intersects the rect.
   void CellsInRect(double x0, double y0, double x1, double y1, std::vector<Cell>& out);

   // Labelled dyes in [x0,x1) x [y0,y1) x [zMin,zMax), world um. Only the
   // 1 um blocks of microtubules that reach the rect are decorated.
   void SitesInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                      std::vector<WorldDye>& out);

   // Blinks of the labelled dyes in the rect/z range with tOn < t1 and
   // tOff > t0 (simulated seconds, see Kinetics). Every blink is a pure
   // function of its dye's address (and, for persistent sites, its time
   // bin), so the answer depends only on (seed, params, kinetics, rect, t0,
   // t1). Appends, ordered by block.
   void EventsInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                       double t0, double t1, std::vector<WorldEvent>& out);

   // Warms the caches for the rect/z range (cells, dye blocks, blink
   // schedules, persistent blinks around [t0, t1)) for up to budgetMs, e.g.
   // a margin around the window, in the time before the next query. Changes
   // no answer. True if the whole region is cached (a repeat over a region
   // already done returns at once), false if the budget ran out first.
   bool Prefetch(double x0, double y0, double x1, double y1, double zMin, double zMax, double t0, double t1,
                 double budgetMs);

   // Kinetics of the blink schedules. Changing them keeps cells, microtubules
   // and dye positions cached and only drops the schedules.
   void SetKinetics(const Kinetics& k);
   const Kinetics& GetKinetics() const { return kin_; }

   // The packed cell of chunk (cx, cy); false if that chunk holds none.
   bool FindCell(int32_t cx, int32_t cy, Cell& out);

   // Labelled-dye counts on an nx x ny grid over the rect (row-major, y
   // outer); returns the total.
   long DensityInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                        int nx, int ny, float* out);

   // Labelled-dye counts on an nx x ny x nz grid over the rect and
   // [zMin, zMax), out[(k*ny + iy)*nx + ix]; plane k spans
   // [zMin + k*(zMax-zMin)/nz, ...). populations: bit 0 bleaching dyes, bit 1
   // persistent sites. Bins straight from the dye blocks (no copy); returns
   // the total. zMin/zMax may be infinite only with nz = 1.
   long Density3dInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                          int nx, int ny, int nz, unsigned populations, float* out);

   // Optical volume (ABI 6, BrightField): per voxel of an nx x ny x nz grid
   // over the rect and [zMin, zMax) (finite), the volume fractions of
   // cytoplasm (cell body minus nucleus), nucleus and microtubule (12.5 nm
   // tubes), channel-major: out[((ch*nz + k)*ny + iy)*nx + ix], ch = 0, 1, 2.
   // Each column is sampled at sub x sub points per voxel footprint; the
   // z overlaps are exact (the body is a height field over the coverslip,
   // the nucleus an ellipsoid). Pure geometry: no optical constants. Cells
   // add (packing keeps them apart). Returns the number of cells that reach
   // the rect.
   long OpticalVolumeInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                              int nx, int ny, int nz, int sub, float* out);

   CellAssets& Assets(const Cell& c);
   void DropCaches();
   const WorldStats& Stats() const { return stats_; }

private:
   struct DyeBlock {
      std::vector<WorldDye> dyes;           // world coordinates
      double zLo = 0, zHi = 0;              // z range of the dyes
      bool scheduled = false;
      std::vector<WorldEvent> events;       // every blink of every bleaching dye, by tOn
      double maxOn = 0;                     // longest of those (tOff - tOn)
      std::vector<uint32_t> persistent;     // indices of the persistent sites
      // Blinks of all persistent sites starting in time bins [pBin0, pBin1),
      // by tOn (PersistentCover extends them a range of bins at a time).
      std::vector<PersistentEvent> pEvents;
      long pBin0 = 0, pBin1 = 0;
      uint64_t used = 0;                    // last query that touched the block
      uint32_t phase = 0;                   // per-block hash, staggers the extensions
      bool generated = false;               // dyes filled in (ForEachDyeBlock)
   };
   using BlockKey = std::array<int32_t, 4>; // cx, cy, mtIndex, block
   struct BlockKeyHash {
      size_t operator()(const BlockKey& k) const
      {
         uint64_t h = 0x9E3779B97F4A7C15ull;
         for (int32_t v : k) {
            h ^= (uint32_t)v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h *= 0xBF58476D1CE4E5B9ull;
         }
         return (size_t)(h ^ (h >> 31));
      }
   };
   // The cells the public CellsInRect returns, as pointers into blocks_
   // (valid until the next block is packed; the walks here pack nothing
   // after it ran).
   void CellsInRectPtr(double x0, double y0, double x1, double y1, std::vector<const Cell*>& out);
   // Makes b.pEvents cover time bins [b0, b1] (the query ends at t1), and
   // extends them ahead of time at a per-block point of their last bin
   // (shortFirst: a first build reaches only one bin ahead). True if it built
   // anything. Touches only b (runs in parallel).
   bool PersistentCover(DyeBlock& b, long b0, long b1, double t1, bool shortFirst) const;
   // Whether PersistentCover(b, b0, b1, t1) would build (the same test).
   bool CoverWouldBuild(const DyeBlock& b, long b0, long b1, double t1) const;

   std::vector<Cell> PackBlock(int32_t bx, int32_t by) const;
   // Builds the assets of those cells not cached yet, in parallel, for the
   // next Assets() calls to take (they keep the usual order).
   void PrebuildAssets(const std::vector<const Cell*>& cells);
   const std::vector<Cell>& PackedBlock(int32_t bx, int32_t by);
   // For every 1 um dye block that can reach the rect/z range, in a fixed
   // order: prep(DyeBlock&) -- work on that block alone, run in parallel
   // over the blocks of a cell (nullptr: none), needs(const DyeBlock&) says
   // whether a block has prep work -- then fn(DyeBlock&), serially, in that
   // order. Missing dye blocks and cell assets are built in parallel first.
   using BlockPrep = std::function<void(DyeBlock&)>;
   using BlockNeeds = std::function<bool(const DyeBlock&)>;
   template <class Fn>
   void ForEachDyeBlock(double x0, double y0, double x1, double y1, double zMin, double zMax,
                        const BlockPrep& prep, const BlockNeeds& needs, Fn fn);
   DyeBlock& FindDyeBlock(const Cell& c, int mtIndex, int block);
   void GenerateDyes(DyeBlock& blk, const Cell& c, const std::vector<Pt3>& pts, const MtFrames& fr, int mtIndex,
                     int block) const;
   // Builds b's blink schedule; true if it did (touches only b).
   bool Schedule(DyeBlock& b) const;
   // The schedule + persistent-cover prep of EventsInWindow/Prefetch, with
   // the build counts added to stats_ afterwards.
   struct PrepCounts { std::atomic<long> schedules{0}, covers{0}; };

   uint32_t seed_;
   Params p_;
   size_t assetCap_;
   size_t dyeCap_;
   std::map<std::pair<int32_t, int32_t>, std::vector<Cell>> blocks_;
   // LRU of cell assets, most recent first
   std::list<std::pair<std::pair<int32_t, int32_t>, std::unique_ptr<CellAssets>>> assets_;
   // LRU of dye blocks (most recent first), bounded by the number of dyes held
   std::list<std::pair<BlockKey, DyeBlock>> dyeLru_;
   std::unordered_map<BlockKey, std::list<std::pair<BlockKey, DyeBlock>>::iterator, BlockKeyHash> dyeIndex_;
   size_t dyeCount_ = 0;
   uint64_t query_ = 0;                    // counts queries, for DyeBlock::used
   uint64_t evictions_ = 0, kinVersion_ = 0;
   // Prefetch: its time limit while it runs (ForEachDyeBlock stops past it)
   // and the last region it completed.
   bool PastDeadline();
   const std::chrono::steady_clock::time_point* deadline_ = nullptr;
   bool stopped_ = false;
   struct PrefetchRegion {
      double x0, y0, x1, y1, zMin, zMax;
      uint64_t evictions, kinVersion;
      bool valid = false;
   };
   PrefetchRegion prefetchDone_ = {};
   Kinetics kin_;
   std::vector<const PersistentEvent*> persistentScratch_;
   // Built ahead in parallel for the current walk (PackedBlock / Assets take
   // them from here instead of building).
   std::map<std::pair<int32_t, int32_t>, std::vector<Cell>> packPrebuilt_;
   std::map<std::pair<int32_t, int32_t>, std::unique_ptr<CellAssets>> assetPrebuilt_;
   WorldStats stats_;
   // The shared worker pool (parallel.h), installed for every public call.
   std::shared_ptr<WorkerPool> pool_;
};

// Cell-local (lx, ly) -> world, as the JS localToWorld (packRot, then x/y).
void LocalToWorld(const Cell& c, double lx, double ly, double& wx, double& wy);

} // namespace isc
