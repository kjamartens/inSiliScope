// Packing: move cells apart (translate + rotate), never shrink; prune the
// lower-priority cell of any pair still overlapping. Port of the JS
// prototype's buildCandidateMap / relax / prune / packMap.
//
// The window is a dense chunk rectangle, so the JS string-keyed Map becomes
// an index grid. Relaxation is Jacobi (every cell updates from the same
// snapshot) and prune collects into a set, so iteration order never affects
// the result; only the per-cell neighbour order (dx outer, dy inner) matters
// for floating-point sums, and that is kept.
#pragma once

#include "cells.h"

#include <cstdint>
#include <vector>

namespace isc {

struct CandidateMap {
   int32_t cx0 = 0, cy0 = 0, cx1 = -1, cy1 = -1;
   std::vector<Cell> cells;       // present candidates, cx-major then cy (JS insertion order)
   std::vector<int32_t> grid;     // (cx-cx0)*H + (cy-cy0) -> index into cells, or -1
   std::vector<uint8_t> alive;    // prune() clears entries

   int Width() const { return cx1 - cx0 + 1; }
   int Height() const { return cy1 - cy0 + 1; }
   int32_t Find(int32_t cx, int32_t cy) const;
};

int InteractionChunks(const Params& p);
CandidateMap BuildCandidateMap(uint32_t seed, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1, const Params& p);
void Relax(CandidateMap& map, const Params& p, int iters);
int Prune(CandidateMap& map, const Params& p);
int PackMap(CandidateMap& map, const Params& p);   // returns number removed

constexpr int PRUNE_ROUNDS = 6;

} // namespace isc
