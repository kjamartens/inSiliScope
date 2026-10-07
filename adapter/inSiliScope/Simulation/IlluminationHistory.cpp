///////////////////////////////////////////////////////////////////////////////
// FILE:          IlluminationHistory.cpp
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   See IlluminationHistory.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "IlluminationHistory.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sim {

namespace {
long FloorDiv(long a, long b)
{
   return a >= 0 ? a / b : -((-a + b - 1) / b);
}
} // namespace

double ClockSnapshot::At(double xUm, double yUm) const
{
   const long ix = static_cast<long>(std::floor(xUm / tileUm)) - ix0;
   const long iy = static_cast<long>(std::floor(yUm / tileUm)) - iy0;
   if (ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny))
      return 0.0;
   return t[static_cast<size_t>(ix) + static_cast<size_t>(nx) * static_cast<size_t>(iy)];
}

void ClockSnapshot::Regions(double x0Um, double y0Um, double x1Um, double y1Um, std::vector<ClockRegion>& out) const
{
   out.clear();
   // Bounding boxes per distinct clock of the tiles meeting the rect (tiles
   // outside the snapshot count as clock 0).
   const long jx0 = static_cast<long>(std::floor(x0Um / tileUm)), jx1 = static_cast<long>(std::floor(x1Um / tileUm));
   const long jy0 = static_cast<long>(std::floor(y0Um / tileUm)), jy1 = static_cast<long>(std::floor(y1Um / tileUm));
   std::map<float, ClockRegion> by;
   for (long jy = jy0; jy <= jy1; ++jy)
      for (long jx = jx0; jx <= jx1; ++jx)
      {
         const long ix = jx - ix0, iy = jy - iy0;
         const float v = ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny)
                            ? 0.0f
                            : t[static_cast<size_t>(ix) + static_cast<size_t>(nx) * static_cast<size_t>(iy)];
         const double tx0 = jx * tileUm, ty0 = jy * tileUm;
         auto it = by.find(v);
         if (it == by.end())
            by[v] = { v, tx0, ty0, tx0 + tileUm, ty0 + tileUm };
         else
         {
            ClockRegion& r = it->second;
            r.x0Um = std::min(r.x0Um, tx0);
            r.y0Um = std::min(r.y0Um, ty0);
            r.x1Um = std::max(r.x1Um, tx0 + tileUm);
            r.y1Um = std::max(r.y1Um, ty0 + tileUm);
         }
      }
   for (auto& kv : by)
      out.push_back(kv.second);
}

void IlluminationHistory::Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec,
                                  const std::function<double(double, double)>& weight)
{
   if (!(dtSec > 0) || !(x1Um > x0Um) || !(y1Um > y0Um))
      return;
   const double T = kTileUm;
   // Tiles whose centre (i + 0.5) T lies in [x0, x1).
   const long ixLo = static_cast<long>(std::ceil(x0Um / T - 0.5)), ixHi = static_cast<long>(std::ceil(x1Um / T - 0.5));
   const long iyLo = static_cast<long>(std::ceil(y0Um / T - 0.5)), iyHi = static_cast<long>(std::ceil(y1Um / T - 0.5));
   std::lock_guard<std::mutex> g(mutex_);
   for (long iy = iyLo; iy < iyHi; ++iy)
      for (long ix = ixLo; ix < ixHi; ++ix)
      {
         const long cx = FloorDiv(ix, kChunk), cy = FloorDiv(iy, kChunk);
         std::vector<float>& c = chunks_[Key(cx, cy)];
         if (c.empty())
            c.assign(static_cast<size_t>(kChunk) * kChunk, 0.0f);
         const double w = weight ? weight((ix + 0.5) * T, (iy + 0.5) * T) : 1.0;
         float& v = c[static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk)];
         v = static_cast<float>(v + dtSec * w);
      }
}

void ClockSnapshot::Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec)
{
   if (!(dtSec > 0) || !(x1Um > x0Um) || !(y1Um > y0Um))
      return;
   const double T = tileUm;
   const long ixLo = static_cast<long>(std::ceil(x0Um / T - 0.5)), ixHi = static_cast<long>(std::ceil(x1Um / T - 0.5));
   const long iyLo = static_cast<long>(std::ceil(y0Um / T - 0.5)), iyHi = static_cast<long>(std::ceil(y1Um / T - 0.5));
   for (long iy = std::max(iyLo, iy0); iy < std::min(iyHi, iy0 + static_cast<long>(ny)); ++iy)
      for (long ix = std::max(ixLo, ix0); ix < std::min(ixHi, ix0 + static_cast<long>(nx)); ++ix)
      {
         float& v = t[static_cast<size_t>(ix - ix0) + static_cast<size_t>(nx) * static_cast<size_t>(iy - iy0)];
         v = static_cast<float>(v + dtSec * 1.0);
      }
}

ClockSnapshot IlluminationHistory::Snapshot(double x0Um, double y0Um, double x1Um, double y1Um) const
{
   ClockSnapshot s;
   s.tileUm = kTileUm;
   s.ix0 = static_cast<long>(std::floor(x0Um / kTileUm));
   s.iy0 = static_cast<long>(std::floor(y0Um / kTileUm));
   const long ix1 = static_cast<long>(std::floor(x1Um / kTileUm)), iy1 = static_cast<long>(std::floor(y1Um / kTileUm));
   s.nx = static_cast<unsigned>(std::max(0L, ix1 - s.ix0 + 1));
   s.ny = static_cast<unsigned>(std::max(0L, iy1 - s.iy0 + 1));
   s.t.assign(static_cast<size_t>(s.nx) * s.ny, 0.0f);
   std::lock_guard<std::mutex> g(mutex_);
   for (unsigned j = 0; j < s.ny; ++j)
      for (unsigned i = 0; i < s.nx; ++i)
      {
         const long ix = s.ix0 + static_cast<long>(i), iy = s.iy0 + static_cast<long>(j);
         const long cx = FloorDiv(ix, kChunk), cy = FloorDiv(iy, kChunk);
         auto it = chunks_.find(Key(cx, cy));
         if (it != chunks_.end())
            s.t[i + static_cast<size_t>(s.nx) * j] =
               it->second[static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk)];
      }
   return s;
}

void IlluminationHistory::Reset()
{
   std::lock_guard<std::mutex> g(mutex_);
   chunks_.clear();
}

size_t IlluminationHistory::Chunks() const
{
   std::lock_guard<std::mutex> g(mutex_);
   return chunks_.size();
}

} // namespace sim
