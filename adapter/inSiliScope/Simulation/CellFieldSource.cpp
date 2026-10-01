///////////////////////////////////////////////////////////////////////////////
// FILE:          CellFieldSource.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See CellFieldSource.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "CellFieldSource.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace sim {

CellFieldSource::~CellFieldSource()
{
   isc_world_free(world_);
}

bool CellFieldSource::Configure(const CellFieldSettings& s, std::string& err)
{
   if (isc_abi_version() != ISC_ABI_VERSION)
   {
      err = "insiliscope core ABI mismatch";
      return false;
   }
   if (!world_ || !settings_.SameWorld(s))
   {
      IscParams* p = isc_params_new();
      if (!p)
      {
         err = "out of memory";
         return false;
      }
      for (const auto& kv : s.params)
      {
         if (isc_params_set(p, kv.first.c_str(), kv.second) != 0)
         {
            isc_params_free(p);
            err = "unknown cell-field parameter " + kv.first;
            return false;
         }
      }
      IscWorld* w = isc_world_new(s.seed, p);
      isc_params_free(p);
      if (!w)
      {
         err = "could not create the cell-field world";
         return false;
      }
      // Keep ~16M dyes (~1.5-3 GB): the whole z column of a 12.8 um FOV plus
      // margin at 100% labelling is ~9M, so focusing through the cell and
      // moving about a FOV in xy come back from the cache, not regenerated.
      // The core's default (2M) suits the browser.
      isc_world_set_dye_cache(w, 16e6);
      isc_world_free(world_);
      world_ = w;
      settings_ = s;
      settings_.activationRatePerSec = -1; // force the kinetics below
   }
   if (!settings_.SameKinetics(s))
   {
      if (isc_world_set_kinetics(world_, s.activationRatePerSec, s.onSec, s.offSec, s.bleachProb, s.photonCV) != 0)
      {
         err = "invalid dye kinetics (activation rate must be >= 0, ON time > 0)";
         return false;
      }
      settings_ = s;
   }
   return true;
}

bool CellFieldSource::Events(const CellFieldQuery& q, std::vector<BlinkEvent>& out)
{
   if (!world_ || !(q.frameSec > 0))
      return false;
   const double inf = std::numeric_limits<double>::infinity();
   const double zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -inf;
   const double zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : inf;
   const double t1 = q.tSec + q.spanSec;
   // The core's cap/total convention: grow and ask again (the second call is
   // served from its caches).
   int32_t cap = static_cast<int32_t>(buf_.size() / ISC_EVENT_STRIDE);
   int32_t n;
   for (;;)
   {
      n = isc_events_in_window(world_, q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec, t1,
                               buf_.empty() ? nullptr : buf_.data(), cap);
      if (n < 0)
         return false;
      if (n <= cap)
         break;
      cap = n + n / 4;
      buf_.resize(static_cast<size_t>(cap) * ISC_EVENT_STRIDE);
   }
   out.reserve(out.size() + static_cast<size_t>(n));
   for (int32_t i = 0; i < n; ++i)
   {
      const double* e = &buf_[static_cast<size_t>(i) * ISC_EVENT_STRIDE];
      BlinkEvent b;
      b.xUm = e[0] - q.originXUm;
      b.yUm = e[1] - q.originYUm;
      b.zNm = (e[2] - q.zRefUm) * 1000.0;
      b.tStart = q.frameIndex + (e[3] - q.tSec) / q.frameSec;
      b.tEnd = q.frameIndex + (e[4] - q.tSec) / q.frameSec;
      b.brightness = e[5];
      out.push_back(b);
   }
   return true;
}

bool CellFieldSource::Prefetch(const CellFieldQuery& q, double marginUm, double budgetMs)
{
   if (!world_ || !(budgetMs > 0))
      return false;
   using Clock = std::chrono::steady_clock;
   const Clock::time_point end = Clock::now() + std::chrono::microseconds(static_cast<long long>(budgetMs * 1000));
   const double inf = std::numeric_limits<double>::infinity();
   // The window's whole z column first (a focus move), then growing xy rings.
   for (double m : {0.0, marginUm / 3.0, marginUm})
   {
      const double left = std::chrono::duration<double, std::milli>(end - Clock::now()).count();
      if (left <= 0)
         return false;
      if (isc_world_prefetch(world_, q.x0Um - m, q.y0Um - m, q.x1Um + m, q.y1Um + m, -inf, inf, q.tSec,
                             q.tSec + q.spanSec, left) != 1)
         return false;
   }
   return true;
}

long CellFieldSource::Density3d(double x0, double y0, double x1, double y1, double zMin, double zMax, int nx,
                                int ny, int nz, int populations, float* out)
{
   if (!world_)
      return -1;
   return isc_density3d_in_window(world_, x0, y0, x1, y1, zMin, zMax, nx, ny, nz, populations, out);
}

long CellFieldSource::OpticalVolume(double x0, double y0, double x1, double y1, double zMin, double zMax, int nx,
                                    int ny, int nz, int sub, float* out)
{
   if (!world_)
      return -1;
   return isc_optical_volume_in_window(world_, x0, y0, x1, y1, zMin, zMax, nx, ny, nz, sub, out);
}

double CellFieldSource::MaxCellHeight(double x0, double y0, double x1, double y1)
{
   if (!world_)
      return -1;
   constexpr int kStride = ISC_CELL_STRIDE; // height at 6
   int32_t cap = 64;
   for (;;)
   {
      buf_.resize(static_cast<size_t>(cap) * kStride);
      const int32_t n = isc_cells_in_window(world_, x0, y0, x1, y1, buf_.data(), cap);
      if (n < 0)
         return -1;
      if (n > cap)
      {
         cap = n;
         continue;
      }
      double h = 0;
      for (int32_t i = 0; i < n; ++i)
         h = std::max(h, buf_[static_cast<size_t>(i) * kStride + 6]);
      return h;
   }
}

} // namespace sim
