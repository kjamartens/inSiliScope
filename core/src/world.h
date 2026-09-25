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
   uint32_t id;                              // per-dye hash (schedule seed)
   int32_t cx, cy, mtIndex;
};

struct WorldStats {
   long blocksPacked = 0, cellsBuilt = 0, framesBuilt = 0, dyeBlocks = 0;
};

class World {
public:
   World(uint32_t seed, const Params& p, size_t assetCacheCells = 48);

   const Params& GetParams() const { return p_; }
   uint32_t Seed() const { return seed_; }

   // Cells (packed pose) whose footprint circle (rOuter) intersects the rect.
   void CellsInRect(double x0, double y0, double x1, double y1, std::vector<Cell>& out);

   // Labelled dyes in [x0,x1) x [y0,y1) x [zMin,zMax), world um. Only the
   // 1 um blocks of microtubules that reach the rect are decorated.
   void SitesInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                      std::vector<WorldDye>& out);

   // Labelled-dye counts on an nx x ny grid over the rect (row-major, y
   // outer); returns the total.
   long DensityInWindow(double x0, double y0, double x1, double y1, double zMin, double zMax,
                        int nx, int ny, float* out);

   CellAssets& Assets(const Cell& c);
   void DropCaches();
   const WorldStats& Stats() const { return stats_; }

private:
   const std::vector<Cell>& PackedBlock(int32_t bx, int32_t by);

   uint32_t seed_;
   Params p_;
   size_t assetCap_;
   std::map<std::pair<int32_t, int32_t>, std::vector<Cell>> blocks_;
   // LRU of cell assets, most recent first
   std::list<std::pair<std::pair<int32_t, int32_t>, std::unique_ptr<CellAssets>>> assets_;
   WorldStats stats_;
};

// Cell-local (lx, ly) -> world, as the JS localToWorld (packRot, then x/y).
void LocalToWorld(const Cell& c, double lx, double ly, double& wx, double& wy);

} // namespace isc
