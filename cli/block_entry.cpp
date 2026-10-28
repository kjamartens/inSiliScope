// The webSMLM block's sample drift (tools/make_cellfield_block.mjs: CellField.driftTrajectory) on the render
// engine's sim::DriftTrajectory: the path an insiliscope movie, the MM adapter and the viewer take for that seed.
// Linked into the block's module next to the core's C ABI (core/src/wasm_entry.cpp).
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "Drift.h"

#include "insiliscope/insiliscope.h"

#include <cmath>
#include <cstdint>

extern "C" {

// The drift of frames f = 0 .. frames - 1 (0 at frame 0), nm, into out (frames x {x, y, z}). seed: the movie's seed
// (taken mod 2^32, as JS ToInt32); settings: DriftSettings in its order (xyNmPerSqrtSec, zNmPerSqrtSec,
// xySpeedNmPerSec, zSpeedNmPerSec, xyAngleDeg, zDirection, angleWanderDeg, zAngleWanderDeg, speedWanderPct,
// wanderTimeSec). Returns frames, -1 on a bad argument.
ISC_API int32_t isc_drift_trajectory(double seed, int32_t frames, double frameSec, const double* settings, double* out)
{
   if (frames < 0 || !settings || (frames > 0 && !out) || !std::isfinite(seed))
      return -1;
   sim::DriftSettings s;
   s.xyNmPerSqrtSec = settings[0];
   s.zNmPerSqrtSec = settings[1];
   s.xySpeedNmPerSec = settings[2];
   s.zSpeedNmPerSec = settings[3];
   s.xyAngleDeg = settings[4];
   s.zDirection = static_cast<int>(settings[5]);
   s.angleWanderDeg = settings[6];
   s.zAngleWanderDeg = settings[7];
   s.speedWanderPct = settings[8];
   s.wanderTimeSec = settings[9];
   const uint32_t u = static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(std::trunc(std::fmod(seed, 4294967296.0)))));
   const std::vector<sim::DriftNm> t = sim::DriftTrajectory(static_cast<long>(static_cast<int32_t>(u)), frames, frameSec, s);
   for (size_t f = 0; f < t.size(); ++f)
   {
      out[3 * f] = t[f].x;
      out[3 * f + 1] = t[f].y;
      out[3 * f + 2] = t[f].z;
   }
   return frames;
}

} // extern "C"
