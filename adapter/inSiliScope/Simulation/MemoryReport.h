///////////////////////////////////////////////////////////////////////////////
// FILE:          MemoryReport.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   What the engine's caches hold, bytes per owner (2026-10-31): the PSF kernel stacks, the
//                mean-field scenes (plane and kernel spectra, dye tiles, images, per-cell arrays), the background
//                maps, the light history. Summed on request (ScopeMemoryReport, LiveScope::Memory); printed as
//                "ISC_MEMORY <owner> <MB>" with ISC_TIMING=1 (cli), JSON for the viewer (isc_memory_json). A
//                shared array is counted once (MemoryReport::Once). Reading it changes no output.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Timing.h"

namespace sim {

template <class T>
double VectorBytes(const std::vector<T>& v)
{
   return static_cast<double>(v.capacity()) * sizeof(T);
}

class MemoryReport
{
public:
   // Adds bytes to the owner's line.
   void Add(const std::string& owner, double bytes)
   {
      for (auto& e : items_)
         if (e.first == owner)
         {
            e.second += bytes;
            return;
         }
      items_.emplace_back(owner, bytes);
   }
   // Whether p is new to this report (an array shared by several holders: count it at the first).
   bool Once(const void* p) { return p && seen_.insert(p).second; }
   double Total() const
   {
      double t = 0.0;
      for (const auto& e : items_)
         t += e.second;
      return t;
   }
   // The owners, largest first.
   std::vector<std::pair<std::string, double>> Items() const
   {
      std::vector<std::pair<std::string, double>> v = items_;
      std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      return v;
   }
   // {"heapMB": h, "totalMB": t, "owners": [["name", MB], ...]} (heapMB: the host's heap, 0 = unknown).
   std::string Json(double heapBytes = 0.0) const
   {
      char b[160];
      std::snprintf(b, sizeof b, "{\"heapMB\": %.1f, \"totalMB\": %.1f, \"owners\": [", heapBytes / 1048576.0,
                    Total() / 1048576.0);
      std::string j = b;
      bool first = true;
      for (const auto& e : Items())
      {
         std::snprintf(b, sizeof b, "%s[\"%s\", %.1f]", first ? "" : ", ", e.first.c_str(), e.second / 1048576.0);
         j += b;
         first = false;
      }
      return j + "]}";
   }
   // ISC_MEMORY lines (with ISC_TIMING=1).
   void Log() const
   {
      if (!TimingPrinting())
         return;
      for (const auto& e : Items())
         std::fprintf(stderr, "ISC_MEMORY %-28s %9.1f MB\n", e.first.c_str(), e.second / 1048576.0);
      std::fprintf(stderr, "ISC_MEMORY %-28s %9.1f MB\n", "total", Total() / 1048576.0);
      std::fflush(stderr);
   }

private:
   std::vector<std::pair<std::string, double>> items_;
   std::set<const void*> seen_;
};

} // namespace sim
