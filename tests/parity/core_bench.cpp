// Per-phase timing of the core (not a check; tools/port_check.sh does not run
// it). Prints where the time of a cold window goes: candidate cells, packing
// relaxation and pruning, the cytoplasm grid and mesh, microtubules, dyes
// and blink schedules, then the steady-state event query per frame.
//
//   isc_core_bench [seed] [windowUm]        (defaults 1249, 12.8)
//   node isc_core_bench.js                  (the WASM build: serial)
//
// The WASM module's phase timing from JavaScript is tools/bench_core.mjs.
#include "cells.h"
#include "cytomesh.h"
#include "dyes.h"
#include "microtubules.h"
#include "packing.h"
#include "parallel.h"
#include "params.h"
#include "world.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace isc;

namespace {

double Ms(std::chrono::steady_clock::time_point t0)
{
   return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Line(const char* what, double ms, const char* extra = "")
{
   std::printf("  %-34s %9.1f ms  %s\n", what, ms, extra);
}

} // namespace

int main(int argc, char** argv)
{
   const uint32_t seed = argc > 1 ? (uint32_t)std::strtoul(argv[1], nullptr, 10) : 1249u;
   const double W = argc > 2 ? std::atof(argv[2]) : 12.8;
   Params p;
   NormalizeParams(p);
   std::printf("core bench: seed %u, window %.1f um, threads %d\n", seed, W, WorldThreads());
   // The worker pool a World installs for its calls (parallel.h), so the
   // standalone packing/asset phases below run as they do inside a World.
   const std::shared_ptr<WorkerPool> pool = AcquireWorkerPool();
   PoolScope scope(pool);

   // --- Packing of block (0, 0), as World::PackBlock does it.
   {
      const int32_t M = InteractionChunks(p) + 2;
      const int32_t cx0 = -M, cy0 = -M, cx1 = PACK_BLOCK_CHUNKS - 1 + M, cy1 = PACK_BLOCK_CHUNKS - 1 + M;
      auto t0 = std::chrono::steady_clock::now();
      CandidateMap m = BuildCandidateMap(seed, cx0, cy0, cx1, cy1, p);
      char b[96];
      std::snprintf(b, sizeof b, "(%d chunks, %zu candidates)", (cx1 - cx0 + 1) * (cy1 - cy0 + 1), m.cells.size());
      Line("pack: RawCandidate map", Ms(t0), b);
      CandidateMap m80 = m;
      t0 = std::chrono::steady_clock::now();
      Relax(m80, p, 80);
      Line("pack: Relax 80 iterations", Ms(t0));
      CandidateMap mAll = m;
      t0 = std::chrono::steady_clock::now();
      const int removed = PackMap(mAll, p);
      std::snprintf(b, sizeof b, "(%d pruned; prune rounds = this - Relax 80)", removed);
      Line("pack: PackMap (relax + prune rounds)", Ms(t0), b);
   }

   // --- Per-cell assets of the first cells near the origin.
   World w(seed, p);
   std::vector<Cell> cells;
   {
      auto t0 = std::chrono::steady_clock::now();
      w.CellsInRect(-W / 2, -W / 2, W / 2, W / 2, cells);
      char b[96];
      std::snprintf(b, sizeof b, "(%zu cells, %ld blocks packed)", cells.size(), w.Stats().blocksPacked);
      Line("world: CellsInRect cold", Ms(t0), b);
   }
   double tOutline = 0, tGrid = 0, tMesh = 0, tGeom = 0, tMts = 0;
   size_t nMts = 0, nPts = 0;
   const size_t nCells = std::min<size_t>(3, cells.size());
   for (size_t i = 0; i < nCells; i++) {
      const Cell& c = cells[i];
      auto t0 = std::chrono::steady_clock::now();
      std::vector<Pt2> outline = CellOutlineLocal(c, CYTO_OUTLINE_N);
      tOutline += Ms(t0);
      t0 = std::chrono::steady_clock::now();
      CytoHeightGrid hg = BuildCytoHeightGrid(c, p);
      tGrid += Ms(t0);
      t0 = std::chrono::steady_clock::now();
      CytoMesh mesh = BuildCytoMesh(c, p);
      tMesh += Ms(t0);
      t0 = std::chrono::steady_clock::now();
      MtCellGeom g = BuildMtCellGeom(c, p);
      tGeom += Ms(t0);
      t0 = std::chrono::steady_clock::now();
      std::vector<Microtubule> mts = BuildMicrotubulesForCell(seed, c, p, g);
      tMts += Ms(t0);
      nMts += mts.size();
      for (const Microtubule& m : mts) nPts += m.pts.size();
      (void)outline; (void)hg; (void)mesh;
   }
   {
      char b[96];
      std::snprintf(b, sizeof b, "(%zu cells)", nCells);
      Line("assets: CellOutlineLocal(2048)", tOutline, b);
      Line("assets: BuildCytoHeightGrid", tGrid, b);
      Line("assets: BuildCytoMesh (incl. grid)", tMesh, b);
      Line("assets: BuildMtCellGeom (incl. mesh)", tGeom, b);
      std::snprintf(b, sizeof b, "(%zu cells, %zu MTs, %zu points)", nCells, nMts, nPts);
      Line("assets: BuildMicrotubulesForCell", tMts, b);
   }

   // --- Dyes and blinks over the window (cells + assets are built by the first query).
   const double INF = std::numeric_limits<double>::infinity();
   {
      std::vector<WorldDye> sites;
      auto t0 = std::chrono::steady_clock::now();
      w.SitesInWindow(-W / 2, -W / 2, W / 2, W / 2, -INF, INF, sites);
      char b[128];
      std::snprintf(b, sizeof b, "(%zu dyes; %ld cells built, %ld dye blocks)", sites.size(), w.Stats().cellsBuilt,
                    w.Stats().dyeBlocks);
      Line("world: SitesInWindow cold (assets + dyes)", Ms(t0), b);
      sites.clear();
      t0 = std::chrono::steady_clock::now();
      w.SitesInWindow(-W / 2, -W / 2, W / 2, W / 2, -INF, INF, sites);
      Line("world: SitesInWindow warm", Ms(t0));
   }
   {
      Kinetics k;
      k.activationRatePerSec = 0.00143;
      w.SetKinetics(k);
      std::vector<WorldEvent> ev;
      auto t0 = std::chrono::steady_clock::now();
      w.EventsInWindow(-W / 2, -W / 2, W / 2, W / 2, -3, 4, 0, 0.05, ev);
      char b[96];
      std::snprintf(b, sizeof b, "(%zu events; %ld schedules)", ev.size(), w.Stats().schedulesBuilt);
      Line("world: EventsInWindow first (schedules)", Ms(t0), b);
      t0 = std::chrono::steady_clock::now();
      size_t n = 0;
      for (int f = 1; f <= 200; f++) {
         ev.clear();
         w.EventsInWindow(-W / 2, -W / 2, W / 2, W / 2, -3, 4, f * 0.05, (f + 1) * 0.05, ev);
         n += ev.size();
      }
      std::snprintf(b, sizeof b, "(200 frames, %zu events)", n);
      Line("world: EventsInWindow steady / frame", Ms(t0) / 200, b);
   }
   {
      std::vector<float> d((size_t)128 * 128 * 280);
      auto t0 = std::chrono::steady_clock::now();
      w.Density3dInWindow(-W / 2, -W / 2, W / 2, W / 2, -3, 4, 128, 128, 280, 3, d.data());
      Line("world: Density3dInWindow 128x128x280", Ms(t0));
      std::vector<float> v((size_t)3 * 128 * 128 * 20);
      t0 = std::chrono::steady_clock::now();
      w.OpticalVolumeInWindow(-W / 2, -W / 2, W / 2, W / 2, -0.5, 9.5, 128, 128, 20, 2, v.data());
      Line("world: OpticalVolume 128x128x20 sub 2", Ms(t0));
   }
   return 0;
}
