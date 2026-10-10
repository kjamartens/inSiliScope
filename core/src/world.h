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

#include "blockstore.h"
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
// The structures that carry labels (issue 16; JS world.js STRUCTURES): one
// label each, indexed by their position. Only the microtubules for now; a new
// structure (nucleus DNA, NPCs, ...) adds an entry and its site generator.
constexpr int STRUCTURE_MT = 0;
constexpr int STRUCTURE_COUNT = 1;

struct CellAssets {
   Cell cell;                                // shape + packed pose
   MtCellGeom geom;                          // direction table, area, cytoplasm mesh
   // The microtubules (and what hangs off them) are built on demand: the
   // viewer's cytoplasm-mesh request does not need them (World::Assets).
   bool mtsBuilt = false;
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
   int32_t structure;                        // STRUCTURE_*
};

// One blink (state STATE_BLINK, aux 0) or continuous window (EventState) of
// one dye, in world um and simulated seconds.
struct WorldEvent {
   double x, y, z;
   double tOn, tOff, brightness;
   double aux;
   uint32_t id;
   uint8_t structure, state;
};

// A persistent-site blink cached in its dye block: dye index in the block,
// and (bin, j) to restore PersistentBlinks' order.
struct PersistentEvent {
   double tOn, tOff, brightness;
   uint32_t dye, bin, j;
};

struct WorldStats {
   long blocksPacked = 0, cellsBuilt = 0, framesBuilt = 0;
   long blocksInjected = 0;     // packed blocks taken from another world (SetPackedBlock)
   long blocksFromStore = 0;    // packed blocks taken from the disk store (SetCacheDir)
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
   World(World&&) = default;
   World& operator=(World&&) = default;
   ~World();   // flushes the block store

   // Packed blocks kept across runs in dir/packed_blocks.bin (blockstore.h;
   // the C ABI's isc_world_set_cache_dir). False if the directory cannot be
   // used; "" turns the store off. Blocks packed so far are not written
   // retroactively.
   bool SetCacheDir(const std::string& dir);
   bool FlushCache();
   const BlockStore* Store() const { return store_.get(); }

   const Params& GetParams() const { return p_; }
   // Dye cache cap (dyes); a smaller one takes effect at the next query.
   void SetDyeCacheCap(size_t dyes) { dyeCap_ = dyes; }
   uint32_t Seed() const { return seed_; }

   // Cells (packed pose) whose footprint circle (rOuter) intersects the rect.
   void CellsInRect(double x0, double y0, double x1, double y1, std::vector<Cell>& out);

   // Fluorescent dyes in [x0,x1) x [y0,y1) x [zMin,zMax), world um. Only the
   // 1 um blocks of microtubules that reach the rect are decorated.
   void SitesInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                      std::vector<WorldDye>& out);

   // Blinks of the fluorescent dyes in the rect/z range with tOn < t1 and
   // tOff > t0 (simulated seconds, see Kinetics), state STATE_BLINK. Every
   // blink is a pure function of its dye's address (and, for DNA-PAINT, its
   // time bin), so the answer depends only on (seed, params, labels, rect,
   // t0, t1). Appends, ordered by block.
   void EventsInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                       double t0, double t1, std::vector<WorldEvent>& out);

   // The continuous windows (LabelSchedule: PRE, INITIAL_ON, ALWAYS_ON with
   // tOff infinite) of the dyes in the rect/z range that end after tMin, dye
   // order per block, brightness 1. Made on each call (not cached).
   void ContinuousInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax, double tMin,
                           std::vector<WorldEvent>& out);

   // Warms the caches for the rect/z range (cells, dye blocks, blink
   // schedules, persistent blinks around [t0, t1)) for up to budgetMs, e.g.
   // a margin around the window, in the time before the next query. Changes
   // no answer. True if the whole region is cached (a repeat over a region
   // already done returns at once), false if the budget ran out first.
   bool Prefetch(double x0, double y0, double x1, double y1, double zMin, double zMax, double t0, double t1,
                 double budgetMs);

   // The label of a structure (STRUCTURE_*). A change of density or
   // fluorescent fraction redraws the dyes (cells and microtubules stay
   // cached); any other change only drops the schedules. False (nothing
   // changes) for a bad structure or an invalid label (ValidateLabel).
   bool SetLabel(int structure, const Label& l);
   const Label& GetLabel(int structure) const { return labels_[(size_t)structure]; }

   // ABI 11: the kinetics history of the following queries (EventsInWindow,
   // ContinuousInWindow, Prefetch): segment starts tStart (tStart[0] = 0,
   // increasing) and per segment the kinetics of every structure, kin[i *
   // STRUCTURE_COUNT + s]. Empty: none (each label's kinetics from t = 0). The
   // dye blocks keep a schedule per history, up to 1 + kParkedSlots (clock
   // regions sharing a block do not rebuild each other; a label change of the
   // kinetics alone keeps those made under a history). False (nothing
   // changes) on bad input.
   bool SetKineticsHistory(const std::vector<double>& tStart, const std::vector<Kinetics>& kin);
   // Structure s's history (nullptr: none).
   const KineticsHistory* HistoryOf(int s) const
   {
      return hist_[(size_t)s].Size() ? &hist_[(size_t)s] : nullptr;
   }

   // Mean emission dipole of a dye from SitesInWindow (DyeOrientation; false
   // for Free), cell-local.
   bool DyeOrientationOf(const WorldDye& d, Pt3& dir);

   // The packed cell of chunk (cx, cy); false if that chunk holds none.
   bool FindCell(int32_t cx, int32_t cy, Cell& out);

   // The packed cells of 8x8-chunk block (bx, by), in the block's order
   // (packed now if not cached).
   const std::vector<Cell>& BlockCells(int32_t bx, int32_t by);
   // Installs block (bx, by) from rows another world of the same seed and
   // params packed (`stride` doubles each: cx, cy, x, y, packRot first, as
   // the C ABI's cell rows); each cell's shape is recomputed from its
   // address. skipped = the block was cached already (nothing changes).
   // False (nothing changes) if a row is not a present cell of the block or
   // the rows are out of the block's (cx, then cy) order. Caches only.
   bool SetPackedBlock(int32_t bx, int32_t by, const double* rows, int32_t n, int stride, bool& skipped);

   // Fluorescent-dye counts on an nx x ny grid over the rect (row-major, y
   // outer), every structure; returns the total.
   long DensityInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                        int nx, int ny, float* out);

   // Fluorescent-dye counts on an nx x ny x nz grid over the rect and
   // [zMin, zMax), out[(k*ny + iy)*nx + ix]; plane k spans
   // [zMin + k*(zMax-zMin)/nz, ...). structureMask: bit s = structure s.
   // Bins straight from the dye blocks (no copy); returns the total.
   // zMin/zMax may be infinite only with nz = 1.
   long Density3dInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                          int nx, int ny, int nz, unsigned structureMask, float* out);

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

   // The cell's assets; withMts = false may leave the microtubules unbuilt
   // (the cytoplasm mesh alone, for the viewer).
   CellAssets& Assets(const Cell& c, bool withMts = true);
   void DropCaches();
   const WorldStats& Stats() const { return stats_; }

private:
   // A dye in its block (issue 16: millions of dyes in a dense FOV, so the
   // cell, microtubule and structure are the block's): world position, id,
   // and the lattice address kn = k << 24 | n.
   struct PackedDye {
      double x, y, z;
      uint32_t id, kn;
      int32_t K() const { return (int32_t)(kn >> 24); }
      int32_t N() const { return (int32_t)(kn & 0xFFFFFFu); }
   };
   struct DyeBlock {
      std::vector<PackedDye> dyes;          // world coordinates
      int32_t cx = 0, cy = 0, mtIndex = 0;
      uint32_t h1 = 0;                      // DyeH1 of the microtubule
      int32_t structure = STRUCTURE_MT;
      double zLo = 0, zHi = 0;              // z range of the dyes
      bool scheduled = false;
      double tLo = 0, horizon = 0;          // the blinks kept: ending after tLo, starting before horizon
      std::vector<WorldEvent> events;       // those blinks (dSTORM, PALM), by tOn
      double maxOn = 0;                     // longest of those (tOff - tOn)
      bool persistent = false;              // every dye a persistent (DNA-PAINT) site
      // Blinks of all persistent sites starting in time bins [pBin0, pBin1),
      // by tOn (PersistentCover extends them a range of bins at a time).
      std::vector<PersistentEvent> pEvents;
      long pBin0 = 0, pBin1 = 0;
      uint64_t used = 0;                    // last query that touched the block
      uint32_t phase = 0;                   // per-block hash, staggers the extensions
      bool generated = false;               // dyes filled in (ForEachDyeBlock)
      // The kinetics history the schedule fields above (scheduled ... pBin1)
      // belong to (its fingerprint, 0 = none), and up to kParkedSlots others
      // (UseHistorySlot swaps them in).
      uint64_t fp = 0;
      struct Slot {
         uint64_t fp = 0;
         bool scheduled = false;
         double tLo = 0, horizon = 0, maxOn = 0;
         std::vector<WorldEvent> events;
         std::vector<PersistentEvent> pEvents;
         long pBin0 = 0, pBin1 = 0;
      };
      std::vector<Slot> parked;
   };
   // A shaped illumination's 1/16 dose steps make up to 16 histories in a FOV.
   static constexpr size_t kParkedSlots = 15;
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
   // Builds b's blink schedule for a query [tLo, tMax) unless the cached one
   // covers it: the blinks ending after tLo and starting before the horizon
   // 2 tMax. A dye's blinks there do not depend on the query (DyeSchedule is
   // sequential from t = 0 and stops at the horizon), so any query history
   // gives the same blinks. True if it built (touches only b).
   bool Schedule(DyeBlock& b, double tLo, double tMax) const;
   bool ScheduleCovers(const DyeBlock& b, double tLo, double tMax) const
   {
      return b.fp == histFp_[(size_t)b.structure] && b.scheduled && b.horizon >= tMax && b.tLo <= tLo;
   }
   // Makes b's schedule fields those of the current history of its structure:
   // a parked slot of that history, else empty ones (the old ones parked).
   // Touches only b.
   void UseHistorySlot(DyeBlock& b) const;
   // The DNA-PAINT bin range of a query [t0, t1) for structure s.
   struct PersistRange { bool persist; long b0, b1; };
   PersistRange PersistFor(int s, double t0, double t1) const;
   // Calls fn(block, dye) for every dye in the rect/z range, in block order.
   template <class Fn>
   void ForEachDye(double x0, double y0, double x1, double y1, double zMin, double zMax, Fn fn);
   static WorldDye DyeAt(const DyeBlock& b, const PackedDye& d)
   {
      return { d.x, d.y, d.z, d.id, b.cx, b.cy, b.mtIndex, d.K(), d.N(), b.structure };
   }
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
   uint64_t evictions_ = 0, labelVersion_ = 0;
   // Prefetch: its time limit while it runs (ForEachDyeBlock stops past it)
   // and the last region it completed.
   bool PastDeadline();
   const std::chrono::steady_clock::time_point* deadline_ = nullptr;
   bool stopped_ = false;
   struct PrefetchRegion {
      double x0, y0, x1, y1, zMin, zMax;
      uint64_t evictions, labelVersion, histVersion;
      bool valid = false;
   };
   PrefetchRegion prefetchDone_ = {};
   std::array<Label, STRUCTURE_COUNT> labels_;
   // The kinetics history per structure (SetKineticsHistory; empty: none),
   // its fingerprint (0: none) and a count of its changes.
   std::array<KineticsHistory, STRUCTURE_COUNT> hist_;
   std::array<uint64_t, STRUCTURE_COUNT> histFp_ = {};
   uint64_t histVersion_ = 0;
   std::vector<const PersistentEvent*> persistentScratch_;
   // Built ahead in parallel for the current walk (PackedBlock / Assets take
   // them from here instead of building).
   std::map<std::pair<int32_t, int32_t>, std::vector<Cell>> packPrebuilt_;
   std::map<std::pair<int32_t, int32_t>, std::unique_ptr<CellAssets>> assetPrebuilt_;
   WorldStats stats_;
   // The shared worker pool (parallel.h), installed for every public call.
   std::shared_ptr<WorkerPool> pool_;
   // The packed-block disk store (SetCacheDir), if any.
   std::unique_ptr<BlockStore> store_;
   // The cells of rows (cx, cy, x, y, packRot, ...; `stride` doubles per
   // row) as SetPackedBlock installs them: validated, rebuilt from their
   // address, the pose from the row. False if a row is not a present cell of
   // block (bx, by) in order.
   bool CellsFromRows(int32_t bx, int32_t by, const double* rows, int32_t n, int stride, std::vector<Cell>& out) const;
   // A stored block's cells, if the store has valid rows for it.
   bool StoredBlock(int32_t bx, int32_t by, std::vector<Cell>& out);
};

// Cell-local (lx, ly) -> world, as the JS localToWorld (packRot, then x/y).
void LocalToWorld(const Cell& c, double lx, double ly, double& wx, double& wy);

} // namespace isc
