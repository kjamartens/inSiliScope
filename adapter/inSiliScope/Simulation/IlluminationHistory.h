///////////////////////////////////////////////////////////////////////////////
// FILE:          IlluminationHistory.h
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   The adapter's world-anchored illumination history: how many
//                seconds of illumination each place of the sample has had
//                (0.25 um tiles, weighted by the illumination profile), so
//                imaging a region bleaches (and uses up) its dyes there and
//                nowhere else: bleach, move away and come back, and it is
//                still dim; a region never lit starts fresh at clock 0.
//                FluorescenceMovie reads each dye's schedule at its tile's
//                clock (ScopeMovie.h DyeClock); the live loop and stacks
//                advance the lit rect (the FOV and its 2 um margin, what a
//                movie renders) after each frame / stack. Not used by the
//                cli or the viewer (their movies start at start-sec).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "ScopeMovie.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sim {

// The clocks of a rect, copied out of the history (no lock while a movie
// reads it). Positions outside the rect read 0.
class ClockSnapshot : public DyeClock
{
public:
   double At(double xUm, double yUm) const override;
   void Regions(double x0Um, double y0Um, double x1Um, double y1Um, std::vector<ClockRegion>& out) const override;
   long ix0 = 0, iy0 = 0;
   unsigned nx = 0, ny = 0;
   double tileUm = 0.25;
   std::vector<float> t;   // nx x ny, x fastest
};

class IlluminationHistory
{
public:
   static constexpr double kTileUm = 0.25;
   // Adds dtSec x weight(tile centre) to every tile whose centre lies in the
   // rect (world um). weight: the illumination profile, peak 1 (empty: 1).
   void Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec,
                const std::function<double(double xUm, double yUm)>& weight);
   ClockSnapshot Snapshot(double x0Um, double y0Um, double x1Um, double y1Um) const;
   void Reset();
   size_t Chunks() const;

private:
   static constexpr int kChunk = 32;   // tiles per chunk side (8 um)
   static uint64_t Key(long cx, long cy)
   {
      return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
   }
   mutable std::mutex mutex_;
   std::unordered_map<uint64_t, std::vector<float>> chunks_;   // kChunk x kChunk seconds, x fastest
};

} // namespace sim
