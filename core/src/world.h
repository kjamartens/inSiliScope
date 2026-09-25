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
#include "params.h"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
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

struct WorldStats {
   long blocksPacked = 0, cellsBuilt = 0, framesBuilt = 0;
   long dyeBlocks = 0;          // 1 um dye blocks generated (cache misses)
   long dyeBlockHits = 0;       // served from the cache
   long schedulesBuilt = 0;     // blocks whose blink schedules were built
};

class World {
public:
   // Caches: cell assets (LRU, cells) and dye blocks (LRU, bounded by the
   // number of dyes they hold; a 12.8 um FOV beside a nucleus is ~400k dyes
   // at the default 10% labelling, ~50 bytes each plus ~60 per blink).
   World(uint32_t seed, const Params& p, size_t assetCacheCells = 48, size_t dyeCacheDyes = 2000000);

   const Params& GetParams() const { return p_; }
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
      std::vector<uint32_t> persistent;     // indices of the persistent sites (blinks made per query)
   };
   using BlockKey = std::array<int32_t, 4>; // cx, cy, mtIndex, block

   const std::vector<Cell>& PackedBlock(int32_t bx, int32_t by);
   // Calls fn(DyeBlock&) for every 1 um dye block that can reach the rect/z range.
   template <class Fn>
   void ForEachDyeBlock(double x0, double y0, double x1, double y1, double zMin, double zMax, Fn fn);
   DyeBlock& GetDyeBlock(CellAssets& A, int mtIndex, int block);
   void Schedule(DyeBlock& b);

   uint32_t seed_;
   Params p_;
   size_t assetCap_;
   size_t dyeCap_;
   std::map<std::pair<int32_t, int32_t>, std::vector<Cell>> blocks_;
   // LRU of cell assets, most recent first
   std::list<std::pair<std::pair<int32_t, int32_t>, std::unique_ptr<CellAssets>>> assets_;
   // LRU of dye blocks (most recent first), bounded by the number of dyes held
   std::list<std::pair<BlockKey, DyeBlock>> dyeLru_;
   std::map<BlockKey, std::list<std::pair<BlockKey, DyeBlock>>::iterator> dyeIndex_;
   size_t dyeCount_ = 0;
   Kinetics kin_;
   std::vector<Blink> blinkScratch_;
   WorldStats stats_;
};

// Cell-local (lx, ly) -> world, as the JS localToWorld (packRot, then x/y).
void LocalToWorld(const Cell& c, double lx, double ly, double& wx, double& wy);

} // namespace isc
