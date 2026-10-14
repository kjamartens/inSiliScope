///////////////////////////////////////////////////////////////////////////////
// FILE:          IlluminationHistory.cpp
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   See IlluminationHistory.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "IlluminationHistory.h"

#include "Excitation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <tuple>
#include <utility>

namespace sim {

namespace {
long FloorDiv(long a, long b)
{
   return a >= 0 ? a / b : -((-a + b - 1) / b);
}

constexpr long kRegionBlock = 8;   // tiles per side of a region block (2 um)
} // namespace


HistoryLight HistoryLightFor(const Excitation& ex, const std::vector<ExcitationLine>& sharesIn, double axisXUm,
                             double axisYUm)
{
   if (ex.Uniform())
      return {};
   // The light at a tile is the same every frame of a pose: kept per tile (a
   // live frame lights ~15000 tiles x 12 bands; the producer and the
   // render-ahead helper share the memo).
   struct Memo
   {
      HistoryLightField field;
      std::mutex m;
      std::unordered_map<uint64_t, std::array<float, 2 * kHistoryBands>> w;   // the light, then the exact light
      Memo(const Excitation& e, const std::vector<ExcitationLine>& sh) : field(e, sh) {}
   };
   const auto memo = std::make_shared<Memo>(ex, sharesIn);
   axisXUm = HistoryAxisUm(axisXUm);
   axisYUm = HistoryAxisUm(axisYUm);
   return [memo, axisXUm, axisYUm](double x, double y, float* w, float* exact) {
      const double T = IlluminationHistory::kTileUm;
      const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(x / T)))) << 32) |
                           static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / T)));
      {
         std::lock_guard<std::mutex> lock(memo->m);
         auto it = memo->w.find(key);
         if (it != memo->w.end())
         {
            std::copy(it->second.begin(), it->second.begin() + kHistoryBands, w);
            std::copy(it->second.begin() + kHistoryBands, it->second.end(), exact);
            return;
         }
      }
      // (A tile's centre: the same x, y each time, so the same values.)
      std::array<float, 2 * kHistoryBands> v;
      memo->field.Fill(x - axisXUm, y - axisYUm, v.data(), v.data() + kHistoryBands);
      std::copy(v.begin(), v.begin() + kHistoryBands, w);
      std::copy(v.begin() + kHistoryBands, v.end(), exact);
      std::lock_guard<std::mutex> lock(memo->m);
      memo->w.emplace(key, v);
   };
}



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

void ClockSnapshot::KeyAtZ(double xUm, double yUm, double zUm, double& tSec, uint32_t& history) const
{
   if (nb <= 1)
   {
      KeyAt(xUm, yUm, tSec, history);
      return;
   }
   const long ix = static_cast<long>(std::floor(xUm / tileUm)) - ix0;
   const long iy = static_cast<long>(std::floor(yUm / tileUm)) - iy0;
   if (ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny))
   {
      tSec = 0.0;
      history = freshNode;
      return;
   }
   const unsigned b = static_cast<unsigned>(std::upper_bound(edges.begin(), edges.end(), zUm) - edges.begin());
   const size_t i = Index(ix, iy, b);
   tSec = t[i];
   history = node[i];
}

double ClockSnapshot::ExcessAtZ(double xUm, double yUm, double zUm) const
{
   if (!anyExcess)
      return 0.0;
   const long ix = static_cast<long>(std::floor(xUm / tileUm)) - ix0;
   const long iy = static_cast<long>(std::floor(yUm / tileUm)) - iy0;
   if (ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny))
      return 0.0;
   const unsigned b =
      nb <= 1 ? 0u : static_cast<unsigned>(std::upper_bound(edges.begin(), edges.end(), zUm) - edges.begin());
   return ex[Index(ix, iy, b)];
}

void ClockSnapshot::Segments(uint32_t history, std::vector<ClockSegment>& out) const
{
   auto it = segments.find(history);
   if (it == segments.end())
      out.clear();
   else
      out = it->second;
}

namespace {
struct Tile
{
   long jx, jy;
   unsigned b;
};

// Emits the tiles' box (key r, its x, y and band range) or, when they fill
// less than half of it and it is larger than a block, its two halves.
template <class BandZ>
void Bisect(const ClockRegion& r, std::vector<Tile>::iterator first, std::vector<Tile>::iterator last, const BandZ& bandZ,
            std::vector<ClockRegion>& out, double tileUm = IlluminationHistory::kTileUm)
{
   if (first == last)
      return;
   long x0 = LONG_MAX, x1 = LONG_MIN, y0 = LONG_MAX, y1 = LONG_MIN;
   for (auto it = first; it != last; ++it)
   {
      x0 = std::min(x0, it->jx);
      x1 = std::max(x1, it->jx);
      y0 = std::min(y0, it->jy);
      y1 = std::max(y1, it->jy);
   }
   const long w = x1 - x0 + 1, h = y1 - y0 + 1;
   // Distinct tiles (a tile is listed once per band it has the key in).
   std::vector<std::pair<long, long>> xy;
   xy.reserve(static_cast<size_t>(last - first));
   for (auto it = first; it != last; ++it)
      xy.emplace_back(it->jy, it->jx);
   std::sort(xy.begin(), xy.end());
   const size_t distinct = static_cast<size_t>(std::unique(xy.begin(), xy.end()) - xy.begin());
   if (2 * distinct >= static_cast<size_t>(w) * static_cast<size_t>(h) || (w <= kRegionBlock && h <= kRegionBlock))
   {
      ClockRegion s = r;
      s.x0Um = x0 * tileUm;
      s.x1Um = (x1 + 1) * tileUm;
      s.y0Um = y0 * tileUm;
      s.y1Um = (y1 + 1) * tileUm;
      s.z0Um = std::numeric_limits<double>::infinity();
      s.z1Um = -std::numeric_limits<double>::infinity();
      for (auto it = first; it != last; ++it)
      {
         double z0, z1;
         bandZ(it->b, z0, z1);
         s.z0Um = std::min(s.z0Um, z0);
         s.z1Um = std::max(s.z1Um, z1);
      }
      out.push_back(s);
      return;
   }
   const bool alongX = w >= h;
   const long mid = alongX ? x0 + w / 2 : y0 + h / 2;
   auto split = std::partition(first, last, [&](const Tile& t) { return (alongX ? t.jx : t.jy) < mid; });
   Bisect(r, first, split, bandZ, out, tileUm);
   Bisect(r, split, last, bandZ, out, tileUm);
}
} // namespace

void ClockSnapshot::Regions(double x0Um, double y0Um, double x1Um, double y1Um, std::vector<ClockRegion>& out) const
{
   out.clear();
   // Bounding boxes per distinct (clock, history) of the tiles meeting the
   // rect (tiles outside the snapshot count as fresh: clock 0), in x, y and
   // the bands' heights. Ordered by clock (one history per clock in a
   // single-epoch session: as before). A key that fills less than half of its
   // box (a beam's unlit corners, the sheet of HILO) is bisected (Bisect), so
   // its queries do not cover the whole FOV each.
   const long jx0 = static_cast<long>(std::floor(x0Um / tileUm)), jx1 = static_cast<long>(std::floor(x1Um / tileUm));
   const long jy0 = static_cast<long>(std::floor(y0Um / tileUm)), jy1 = static_cast<long>(std::floor(y1Um / tileUm));
   const double inf = std::numeric_limits<double>::infinity();
   struct Box
   {
      ClockRegion r;
      long tiles = 0, lastJx = LONG_MIN, lastJy = LONG_MIN;
   };
   std::map<std::pair<float, uint32_t>, Box> by;
   auto keyAt = [&](long jx, long jy, unsigned b, float& v, uint32_t& h) {
      const long ix = jx - ix0, iy = jy - iy0;
      const bool outside = ix < 0 || iy < 0 || ix >= static_cast<long>(nx) || iy >= static_cast<long>(ny);
      v = outside ? 0.0f : t[Index(ix, iy, b)];
      h = outside ? freshNode : node[Index(ix, iy, b)];
   };
   auto bandZ = [&](unsigned b, double& z0, double& z1) {
      z0 = nb <= 1 || b == 0 ? -inf : edges[b - 1];
      z1 = nb <= 1 || b + 1 >= nb ? inf : edges[b];
   };
   for (unsigned b = 0; b < nb; ++b)
      for (long jy = jy0; jy <= jy1; ++jy)
         for (long jx = jx0; jx <= jx1; ++jx)
         {
            float v;
            uint32_t h;
            keyAt(jx, jy, b, v, h);
            const double tx0 = jx * tileUm, ty0 = jy * tileUm;
            double z0, z1;
            bandZ(b, z0, z1);
            auto it = by.find(std::make_pair(v, h));
            if (it == by.end())
            {
               Box bx;
               bx.r.tSec = v;
               bx.r.x0Um = tx0;
               bx.r.y0Um = ty0;
               bx.r.x1Um = tx0 + tileUm;
               bx.r.y1Um = ty0 + tileUm;
               bx.r.z0Um = z0;
               bx.r.z1Um = z1;
               bx.r.history = h;
               bx.tiles = 1;
               bx.lastJx = jx;
               bx.lastJy = jy;
               by[std::make_pair(v, h)] = bx;
            }
            else
            {
               Box& bx = it->second;
               ClockRegion& r = bx.r;
               r.x0Um = std::min(r.x0Um, tx0);
               r.y0Um = std::min(r.y0Um, ty0);
               r.x1Um = std::max(r.x1Um, tx0 + tileUm);
               r.y1Um = std::max(r.y1Um, ty0 + tileUm);
               r.z0Um = std::min(r.z0Um, z0);
               r.z1Um = std::max(r.z1Um, z1);
               if (!(jx == bx.lastJx && jy == bx.lastJy))
                  bx.tiles++;   // (a tile counted once per band it has the key in: an upper bound)
               bx.lastJx = jx;
               bx.lastJy = jy;
            }
         }
   for (auto& kv : by)
   {
      const ClockRegion& r = kv.second.r;
      const double boxTiles = (r.x1Um - r.x0Um) / tileUm * ((r.y1Um - r.y0Um) / tileUm);
      const bool split = kv.second.tiles < 0.5 * boxTiles && (r.x1Um - r.x0Um > kRegionBlock * tileUm ||
                                                              r.y1Um - r.y0Um > kRegionBlock * tileUm);
      if (!split)
      {
         out.push_back(r);
         continue;
      }
      // The key's tiles (and the band each has it in), halved along the
      // longer side of their box until a piece fills at least half of its
      // box or is a block (2 um): a beam's unlit corners are four rects, a
      // ring a few dozen. The pieces split x or y, so each dye is in one.
      std::vector<Tile> tiles;
      for (unsigned b = 0; b < nb; ++b)
         for (long jy = jy0; jy <= jy1; ++jy)
            for (long jx = jx0; jx <= jx1; ++jx)
            {
               float v;
               uint32_t h;
               keyAt(jx, jy, b, v, h);
               if (v == r.tSec && h == r.history)
                  tiles.push_back({ jx, jy, b });
            }
      Bisect(r, tiles.begin(), tiles.end(), bandZ, out);
   }
}

void ClockSnapshot::Expand()
{
   if (nb > 1)
      return;
   const size_t n = static_cast<size_t>(nx) * ny;
   if (ex.size() != t.size())
      ex.assign(t.size(), 0.0f);
   std::vector<float> t2(n * kHistoryBands), e2(n * kHistoryBands);
   std::vector<uint32_t> n2(n * kHistoryBands);
   for (unsigned b = 0; b < static_cast<unsigned>(kHistoryBands); ++b)
   {
      std::copy(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(n), t2.begin() + static_cast<std::ptrdiff_t>(n * b));
      std::copy(ex.begin(), ex.begin() + static_cast<std::ptrdiff_t>(n), e2.begin() + static_cast<std::ptrdiff_t>(n * b));
      std::copy(node.begin(), node.begin() + static_cast<std::ptrdiff_t>(n), n2.begin() + static_cast<std::ptrdiff_t>(n * b));
   }
   t.swap(t2);
   ex.swap(e2);
   node.swap(n2);
   nb = kHistoryBands;
   edges = HistoryBandEdges();
}

void ClockSnapshot::Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec, const HistoryLight& light)
{
   if (!(dtSec > 0) || !(x1Um > x0Um) || !(y1Um > y0Um))
      return;
   const double T = tileUm;
   const long ixLo = static_cast<long>(std::ceil(x0Um / T - 0.5)), ixHi = static_cast<long>(std::ceil(x1Um / T - 0.5));
   const long iyLo = static_cast<long>(std::ceil(y0Um / T - 0.5)), iyHi = static_cast<long>(std::ceil(y1Um / T - 0.5));
   float w[kHistoryBands], wx[kHistoryBands];
   std::fill(w, w + kHistoryBands, 1.0f);
   std::fill(wx, wx + kHistoryBands, 1.0f);
   if (ex.size() != t.size())
      ex.assign(t.size(), 0.0f);
   for (long iy = std::max(iyLo, iy0); iy < std::min(iyHi, iy0 + static_cast<long>(ny)); ++iy)
      for (long ix = std::max(ixLo, ix0); ix < std::min(ixHi, ix0 + static_cast<long>(nx)); ++ix)
      {
         if (light)
            light((ix + 0.5) * T, (iy + 0.5) * T, w, wx);
         if (nb <= 1)
         {
            bool alike = true;
            for (int b = 1; b < kHistoryBands && alike; ++b)
               alike = w[b] == w[0] && wx[b] == wx[0];
            if (!alike)
               Expand();   // the light differs in height: every band from now on
         }
         for (unsigned b = 0; b < nb; ++b)
         {
            const size_t i = Index(ix - ix0, iy - iy0, b);
            t[i] = static_cast<float>(t[i] + dtSec * w[b]);
            if (wx[b] != w[b])
            {
               ex[i] = static_cast<float>(ex[i] + dtSec * (static_cast<double>(wx[b]) - w[b]));
               anyExcess = anyExcess || ex[i] != 0.0f;
            }
         }
      }
}

void IlluminationHistory::Chunk::Expand()
{
   if (banded)
      return;
   t.resize(kChunkTiles * kHistoryBands);
   ex.resize(kChunkTiles * kHistoryBands);
   node.resize(kChunkTiles * kHistoryBands);
   for (size_t b = 1; b < static_cast<size_t>(kHistoryBands); ++b)
   {
      std::copy(t.begin(), t.begin() + kChunkTiles, t.begin() + static_cast<std::ptrdiff_t>(kChunkTiles * b));
      std::copy(ex.begin(), ex.begin() + kChunkTiles, ex.begin() + static_cast<std::ptrdiff_t>(kChunkTiles * b));
      std::copy(node.begin(), node.begin() + kChunkTiles, node.begin() + static_cast<std::ptrdiff_t>(kChunkTiles * b));
   }
   banded = true;
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
                                  const HistoryLight& light, uint32_t epoch)
{
   if (!(dtSec > 0) || !(x1Um > x0Um) || !(y1Um > y0Um))
      return;
   const double T = kTileUm;
   // Tiles whose centre (i + 0.5) T lies in [x0, x1).
   const long ixLo = static_cast<long>(std::ceil(x0Um / T - 0.5)), ixHi = static_cast<long>(std::ceil(x1Um / T - 0.5));
   const long iyLo = static_cast<long>(std::ceil(y0Um / T - 0.5)), iyHi = static_cast<long>(std::ceil(y1Um / T - 0.5));
   float w[kHistoryBands], wx[kHistoryBands];
   std::fill(w, w + kHistoryBands, 1.0f);
   std::fill(wx, wx + kHistoryBands, 1.0f);
   std::lock_guard<std::mutex> g(mutex_);
   Chunk* chunk = nullptr;
   long lastCx = LONG_MIN, lastCy = LONG_MIN;
   for (long iy = iyLo; iy < iyHi; ++iy)
      for (long ix = ixLo; ix < ixHi; ++ix)
      {
         const long cx = FloorDiv(ix, kChunk), cy = FloorDiv(iy, kChunk);
         if (!(cx == lastCx && cy == lastCy))
         {
            chunk = &chunks_[Key(cx, cy)];   // (an unordered_map: the pointer stays valid as chunks are added)
            lastCx = cx;
            lastCy = cy;
         }
         Chunk& c = *chunk;
         if (c.t.empty())
         {
            c.t.assign(kChunkTiles, 0.0f);
            c.ex.assign(kChunkTiles, 0.0f);
            c.node.assign(kChunkTiles, 0u);
         }
         if (light)
         {
            light((ix + 0.5) * T, (iy + 0.5) * T, w, wx);
            if (!c.banded)
            {
               bool alike = true;
               for (int b = 1; b < kHistoryBands && alike; ++b)
                  alike = w[b] == w[0] && wx[b] == wx[0];
               if (!alike)
                  c.Expand();   // the light differs in height here: every band from now on
            }
         }
         const size_t i = static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk);
         const int nb = c.banded ? kHistoryBands : 1;
         for (int b = 0; b < nb; ++b)
         {
            const size_t k = i + kChunkTiles * static_cast<size_t>(b);
            float& v = c.t[k];
            c.node[k] = Continue(c.node[k], v, epoch);
            v = static_cast<float>(v + dtSec * w[b]);
            if (wx[b] != w[b])
               c.ex[k] = static_cast<float>(c.ex[k] + dtSec * (static_cast<double>(wx[b]) - w[b]));
         }
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
   const size_t n = static_cast<size_t>(s.nx) * s.ny;
   std::lock_guard<std::mutex> g(mutex_);
   // Every band alike unless a chunk the rect meets keeps bands (then compared per tile below).
   bool anyBanded = false;
   for (long cy = FloorDiv(s.iy0, kChunk); cy <= FloorDiv(iy1, kChunk) && !anyBanded; ++cy)
      for (long cx = FloorDiv(s.ix0, kChunk); cx <= FloorDiv(ix1, kChunk) && !anyBanded; ++cx)
      {
         auto it = chunks_.find(Key(cx, cy));
         anyBanded = it != chunks_.end() && it->second.banded;
      }
   const unsigned nbRead = anyBanded ? static_cast<unsigned>(kHistoryBands) : 1u;
   s.nb = nbRead;
   s.t.assign(n * nbRead, 0.0f);
   s.ex.assign(n * nbRead, 0.0f);
   s.node.assign(n * nbRead, 0u);
   s.freshNode = Continue(0, 0.0f, epoch);
   uint32_t lastIn = UINT32_MAX, lastOut = 0;
   float lastT = 0;
   bool banded = false;
   const Chunk* chunk = nullptr;
   long lastCx = LONG_MIN, lastCy = LONG_MIN;
   for (unsigned j = 0; j < s.ny; ++j)
      for (unsigned i = 0; i < s.nx; ++i)
      {
         const long ix = s.ix0 + static_cast<long>(i), iy = s.iy0 + static_cast<long>(j);
         const long cx = FloorDiv(ix, kChunk), cy = FloorDiv(iy, kChunk);
         if (!(cx == lastCx && cy == lastCy))
         {
            auto it = chunks_.find(Key(cx, cy));
            chunk = it == chunks_.end() ? nullptr : &it->second;
            lastCx = cx;
            lastCy = cy;
         }
         const size_t k0 = i + static_cast<size_t>(s.nx) * j;
         const size_t c0 = static_cast<size_t>(ix - cx * kChunk) + static_cast<size_t>(kChunk) * (iy - cy * kChunk);
         for (unsigned b = 0; b < nbRead; ++b)
         {
            const size_t k = k0 + n * b;
            if (!chunk)
               s.node[k] = s.freshNode;
            else
            {
               const size_t ci = c0 + kChunkTiles * (chunk->banded ? b : 0);
               const float v = chunk->t[ci];
               const uint32_t nd = chunk->node[ci];
               s.t[k] = v;
               s.ex[k] = chunk->ex[ci];
               s.anyExcess = s.anyExcess || s.ex[k] != 0.0f;
               // Neighbouring tiles mostly share a past: reuse the last answer.
               if (!(nd == lastIn && v == lastT))
               {
                  lastIn = nd;
                  lastT = v;
                  lastOut = Continue(nd, v, epoch);
               }
               s.node[k] = lastOut;
            }
            if (b > 0 && !banded)
               banded = s.t[k] != s.t[k - n * b] || s.node[k] != s.node[k - n * b] || s.ex[k] != s.ex[k - n * b];
         }
      }
   if (banded)
      s.edges = HistoryBandEdges();
   else if (s.nb > 1)
   {
      // Every band alike (epi): one, the old layout.
      s.nb = 1;
      s.t.resize(n);
      s.ex.resize(n);
      s.node.resize(n);
   }
   // The segments of every history present (and the fresh one).
   std::vector<uint32_t> present;
   for (uint32_t h : s.node)
      if (present.empty() || present.back() != h)
         present.push_back(h);
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
