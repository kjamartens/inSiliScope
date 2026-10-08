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
#include <cstring>
#include <map>
#include <utility>

namespace sim {

namespace {
long FloorDiv(long a, long b)
{
   return a >= 0 ? a / b : -((-a + b - 1) / b);
}
} // namespace

double ClockSnapshot::At(double xUm, double yUm) const
{
   double tSec = 0;
   uint32_t h = 0;
   KeyAt(xUm, yUm, tSec, h);
   return tSec;
}

void ClockSnapshot::KeyAt(double xUm, double yUm, double& tSec, uint32_t& history) const
{
   const long ix = static_cast<long>(std::floor(xUm / tileUm)) - ix0;
   const long iy = static_cast<long>(std::floor(yUm / tileUm)) - iy0;
   if (ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny))
   {
      tSec = 0.0;
      history = freshNode;
      return;
   }
   const size_t i = static_cast<size_t>(ix) + static_cast<size_t>(nx) * static_cast<size_t>(iy);
   tSec = t[i];
   history = node[i];
}

void ClockSnapshot::Segments(uint32_t history, std::vector<ClockSegment>& out) const
{
   auto it = segments.find(history);
   if (it == segments.end())
      out.clear();
   else
      out = it->second;
}

void ClockSnapshot::Regions(double x0Um, double y0Um, double x1Um, double y1Um, std::vector<ClockRegion>& out) const
{
   out.clear();
   // Bounding boxes per distinct (clock, history) of the tiles meeting the
   // rect (tiles outside the snapshot count as fresh: clock 0). Ordered by
   // clock (one history per clock in a single-epoch session: as before).
   const long jx0 = static_cast<long>(std::floor(x0Um / tileUm)), jx1 = static_cast<long>(std::floor(x1Um / tileUm));
   const long jy0 = static_cast<long>(std::floor(y0Um / tileUm)), jy1 = static_cast<long>(std::floor(y1Um / tileUm));
   std::map<std::pair<float, uint32_t>, ClockRegion> by;
   for (long jy = jy0; jy <= jy1; ++jy)
      for (long jx = jx0; jx <= jx1; ++jx)
      {
         const long ix = jx - ix0, iy = jy - iy0;
         const bool outside = ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny);
         const size_t i = outside ? 0 : static_cast<size_t>(ix) + static_cast<size_t>(nx) * static_cast<size_t>(iy);
         const float v = outside ? 0.0f : t[i];
         const uint32_t h = outside ? freshNode : node[i];
         const double tx0 = jx * tileUm, ty0 = jy * tileUm;
         const auto key = std::make_pair(v, h);
         auto it = by.find(key);
         if (it == by.end())
         {
            ClockRegion r;
            r.tSec = v;
            r.x0Um = tx0;
            r.y0Um = ty0;
            r.x1Um = tx0 + tileUm;
            r.y1Um = ty0 + tileUm;
            r.history = h;
            by[key] = r;
         }
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

uint32_t IlluminationHistory::RegisterEpoch(const KineticEnv& env)
{
   std::lock_guard<std::mutex> g(mutex_);
   for (size_t i = 0; i < epochs_.size(); ++i)
      if (*epochs_[i] == env)
         return static_cast<uint32_t>(i);
   epochs_.push_back(std::make_shared<const KineticEnv>(env));
   return static_cast<uint32_t>(epochs_.size() - 1);
}

uint32_t IlluminationHistory::Intern(uint32_t parent, float tStart, uint32_t epoch)
{
   uint32_t bits;
   std::memcpy(&bits, &tStart, sizeof bits);
   const uint64_t key = (static_cast<uint64_t>(parent) * 0x9E3779B97F4A7C15ull) ^
                        (static_cast<uint64_t>(bits) << 21) ^ (static_cast<uint64_t>(epoch) * 0xC2B2AE3D27D4EB4Full);
   auto range = nodeByKey_.equal_range(key);
   for (auto it = range.first; it != range.second; ++it)
   {
      const Node& n = nodes_[it->second];
      if (n.parent == parent && n.tStart == tStart && n.epoch == epoch)
         return it->second;
   }
   Node n;
   n.parent = parent;
   n.tStart = tStart;
   n.epoch = epoch;
   nodes_.push_back(n);
   const uint32_t id = static_cast<uint32_t>(nodes_.size() - 1);
   nodeByKey_.emplace(key, id);
   return id;
}

uint32_t IlluminationHistory::Continue(uint32_t node, float t, uint32_t epoch)
{
   if (node == 0)
      return Intern(0, 0.0f, epoch);   // a fresh place: epoch from 0
   const Node n = nodes_[node];
   if (n.epoch == epoch)
      return node;
   if (t == n.tStart)
   {
      // Its last segment is empty (nothing lit since): replace it, or drop it
      // when the epoch before is the one asked for.
      if (n.parent != 0 && nodes_[n.parent].epoch == epoch)
         return n.parent;
      return Intern(n.parent, n.tStart, epoch);
   }
   return Intern(node, t, epoch);
}

void IlluminationHistory::SegmentsOf(uint32_t node, std::vector<ClockSegment>& out) const
{
   out.clear();
   for (uint32_t k = node; k != 0; k = nodes_[k].parent)
      out.push_back({ static_cast<double>(nodes_[k].tStart), epochs_[nodes_[k].epoch].get() });
   std::reverse(out.begin(), out.end());
}

void IlluminationHistory::Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec,
                                  const std::function<double(double, double)>& weight, uint32_t epoch)
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
         Chunk& c = chunks_[Key(cx, cy)];
         if (c.t.empty())
         {
            c.t.assign(static_cast<size_t>(kChunk) * kChunk, 0.0f);
            c.node.assign(static_cast<size_t>(kChunk) * kChunk, 0u);
         }
         const double w = weight ? weight((ix + 0.5) * T, (iy + 0.5) * T) : 1.0;
         const size_t i = static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk);
         float& v = c.t[i];
         c.node[i] = Continue(c.node[i], v, epoch);
         v = static_cast<float>(v + dtSec * w);
      }
}

ClockSnapshot IlluminationHistory::Snapshot(double x0Um, double y0Um, double x1Um, double y1Um, uint32_t epoch)
{
   ClockSnapshot s;
   s.tileUm = kTileUm;
   s.ix0 = static_cast<long>(std::floor(x0Um / kTileUm));
   s.iy0 = static_cast<long>(std::floor(y0Um / kTileUm));
   const long ix1 = static_cast<long>(std::floor(x1Um / kTileUm)), iy1 = static_cast<long>(std::floor(y1Um / kTileUm));
   s.nx = static_cast<unsigned>(std::max(0L, ix1 - s.ix0 + 1));
   s.ny = static_cast<unsigned>(std::max(0L, iy1 - s.iy0 + 1));
   s.t.assign(static_cast<size_t>(s.nx) * s.ny, 0.0f);
   s.node.assign(static_cast<size_t>(s.nx) * s.ny, 0u);
   std::lock_guard<std::mutex> g(mutex_);
   s.freshNode = Continue(0, 0.0f, epoch);
   uint32_t lastIn = UINT32_MAX, lastOut = 0;
   float lastT = 0;
   for (unsigned j = 0; j < s.ny; ++j)
      for (unsigned i = 0; i < s.nx; ++i)
      {
         const long ix = s.ix0 + static_cast<long>(i), iy = s.iy0 + static_cast<long>(j);
         const long cx = FloorDiv(ix, kChunk), cy = FloorDiv(iy, kChunk);
         auto it = chunks_.find(Key(cx, cy));
         const size_t k = i + static_cast<size_t>(s.nx) * j;
         if (it == chunks_.end())
         {
            s.node[k] = s.freshNode;
            continue;
         }
         const size_t ci = static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk);
         const float v = it->second.t[ci];
         const uint32_t nd = it->second.node[ci];
         s.t[k] = v;
         // Neighbouring tiles mostly share a past: reuse the last answer.
         if (!(nd == lastIn && v == lastT))
         {
            lastIn = nd;
            lastT = v;
            lastOut = Continue(nd, v, epoch);
         }
         s.node[k] = lastOut;
      }
   // The segments of every history present (and the fresh one).
   std::vector<uint32_t> present(s.node);
   present.push_back(s.freshNode);
   std::sort(present.begin(), present.end());
   present.erase(std::unique(present.begin(), present.end()), present.end());
   std::vector<uint32_t> usedEpochs;
   for (uint32_t h : present)
   {
      std::vector<ClockSegment>& segs = s.segments[h];
      SegmentsOf(h, segs);
      for (uint32_t k = h; k != 0; k = nodes_[k].parent)
         usedEpochs.push_back(nodes_[k].epoch);
   }
   std::sort(usedEpochs.begin(), usedEpochs.end());
   usedEpochs.erase(std::unique(usedEpochs.begin(), usedEpochs.end()), usedEpochs.end());
   for (uint32_t e : usedEpochs)
      s.envs.push_back(epochs_[e]);
   return s;
}

double IlluminationHistory::ClockAt(double xUm, double yUm) const
{
   const long ix = static_cast<long>(std::floor(xUm / kTileUm)), iy = static_cast<long>(std::floor(yUm / kTileUm));
   const long cx = FloorDiv(ix, kChunk), cy = FloorDiv(iy, kChunk);
   std::lock_guard<std::mutex> g(mutex_);
   auto it = chunks_.find(Key(cx, cy));
   if (it == chunks_.end())
      return 0.0;
   return it->second.t[static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk)];
}

void IlluminationHistory::Reset()
{
   std::lock_guard<std::mutex> g(mutex_);
   chunks_.clear();
   nodes_.assign(1, Node());
   nodeByKey_.clear();
   // The epochs stay (ids a frame in flight holds remain valid).
}

size_t IlluminationHistory::Chunks() const
{
   std::lock_guard<std::mutex> g(mutex_);
   return chunks_.size();
}

} // namespace sim
