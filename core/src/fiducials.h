// Fiducial markers (beads) on any sample: where they are and how big (the model: spec/ALGORITHM.md "Fiducial
// beads"). The engine owns their light (Simulation/Fiducials.*).
// BSD-3-Clause.
//
// A Poisson field (no hard core) drawn per FID_TILE_UM tile: the tile's count on channel FID_CH_COUNT, candidate i on
// channel FID_CH_CAND0 + 2 i (x, y, the diameter's two Box-Muller uniforms) and its Layer height on FID_CH_CAND0 + 2 i
// + 1, all from Pcg4d(seed ^ FID_SALT, tx, ty, channel). Heights: Coverslip (centre at r), OnTop (the sample's surface
// + r), Layer (uniform in [r, fidLayerUm - r]; a candidate below the surface + r is dropped).
//
// Clumps (ABI 17): a bead is stuck to the next one with chance p = fidAggregationPct / 100, so a clump holds n beads
// with P(n) = (1 - p) p^(n-1) (geometric, mean 1 / (1 - p); the size spread of constant-rate coagulation; estimate),
// at most FID_MAX_CLUMP; fidDensity counts beads, so the candidates (clumps) come at fidDensity (1 - p). Candidate i is
// the clump's first bead; its size is drawn from Pcg4d(seed ^ FID_CLUMP_SALT, tx, ty, FID_MAX_CLUMP i).a, and bead j
// >= 1 touches a bead k < j already placed (k, the direction and the diameter from the same stream at
// FID_TRIES (FID_MAX_CLUMP i + j) + t, try t), the centres rk + rj apart: beside it in the coverslip plane (Coverslip,
// OnTop: both at their radii, so 2 sqrt(rk rj) apart in x, y; OnTop at its own height there) or anywhere around it
// (Layer: kept in the layer and out of the cells), at the first try that overlaps no placed bead and stays
// within FID_TILE_UM of the first bead in x and y (so a query pads by one tile); none: the bead is left out. p = 0
// draws nothing more: the beads of before.
#pragma once

#include "params.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace isc {

constexpr double FID_TILE_UM = 2.0;
constexpr int FID_MAX_PER_TILE = 4096;
constexpr uint32_t FID_SALT = 0x46494455u;   // 'FIDU'
constexpr uint32_t FID_CH_COUNT = 0, FID_CH_CAND0 = 16;
constexpr uint32_t FID_CLUMP_SALT = 0x434c554du;   // 'CLUM'
constexpr int FID_MAX_CLUMP = 64;
constexpr int FID_TRIES = 8;
constexpr double FID_MAX_AGGREGATION_PCT = 90;
enum FiducialHeight { FID_HEIGHT_COVERSLIP = 0, FID_HEIGHT_ON_TOP = 1, FID_HEIGHT_LAYER = 2, FID_HEIGHT_COUNT = 3 };

// Clamps the fid* params as fiducials.js normalizeFiducialParams.
void NormalizeFiducialParams(Params& p);

struct FiducialBead {
   int32_t tx, ty, i;   // address: tile, candidate index (a clump's beads share it)
   double x, y, z;      // centre, world um (z above the coverslip)
   double d;            // diameter, nm
};

// The field (cached per tile; a pure function of seed, params and the sample's surface). Thread-safe.
class FiducialField {
public:
   // surfaceAt(x, y): the sample's surface height there, um (World::SurfaceHeightAt; OnTop and Layer).
   FiducialField(uint32_t seed, const Params& p, std::function<double(double, double)> surfaceAt);
   bool On() const { return p_.fidDensity > 0; }
   // Beads whose centre lies in [x0, x1) x [y0, y1): tx outer, ty inner, candidate order.
   void InRect(double x0, double y0, double x1, double y1, std::vector<FiducialBead>& out);

private:
   const std::vector<FiducialBead>& Tile(int32_t tx, int32_t ty);
   uint32_t seed_;
   Params p_;
   std::function<double(double, double)> surfaceAt_;
   std::mutex mu_;
   std::map<std::pair<int32_t, int32_t>, std::vector<FiducialBead>> tiles_;
};

} // namespace isc
