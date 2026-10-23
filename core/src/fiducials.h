// Fiducial markers (beads) on any sample: where they are and how big. The port of the field in
// web/prototype/scope/fiducials.js (its header is the model). The engine owns their light (Simulation/Fiducials.*).
// BSD-3-Clause.
//
// A Poisson field (no hard core) drawn per FID_TILE_UM tile: the tile's count on channel FID_CH_COUNT, candidate i on
// channel FID_CH_CAND0 + 2 i (x, y, the diameter's two Box-Muller uniforms) and its Layer height on FID_CH_CAND0 + 2 i
// + 1, all from Pcg4d(seed ^ FID_SALT, tx, ty, channel). Heights: Coverslip (centre at r), OnTop (the sample's surface
// + r), Layer (uniform in [r, fidLayerUm - r]; a candidate below the surface + r is dropped).
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
enum FiducialHeight { FID_HEIGHT_COVERSLIP = 0, FID_HEIGHT_ON_TOP = 1, FID_HEIGHT_LAYER = 2, FID_HEIGHT_COUNT = 3 };

// Clamps the fid* params as fiducials.js normalizeFiducialParams.
void NormalizeFiducialParams(Params& p);

struct FiducialBead {
   int32_t tx, ty, i;   // address: tile, candidate index
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
