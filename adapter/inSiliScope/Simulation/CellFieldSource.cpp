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

int CellFieldSettings::LabelMode(int s) const
{
   if (s < 0 || static_cast<size_t>(s) >= labels.size() || labels[static_cast<size_t>(s)].size() <= ISC_LABEL_MODE)
      return ISC_MODE_DNA_PAINT;
   return static_cast<int>(labels[static_cast<size_t>(s)][ISC_LABEL_MODE]);
}

std::vector<double> MakeLabelVector(int mode, double density, double fluorescentFraction, double activationRatePerSec,
                                    double onSec, double offSec, double bleachProb, double photonCV,
                                    double initialOnSec, bool preState)
{
   std::vector<double> v(ISC_LABEL_COUNT, 0.0);
   v[ISC_LABEL_DENSITY] = density;
   v[ISC_LABEL_FLUORESCENT_FRACTION] = fluorescentFraction;
   v[ISC_LABEL_MODE] = mode;
   v[ISC_LABEL_ACTIVATION_RATE] = activationRatePerSec;
   v[ISC_LABEL_ON_SEC] = onSec;
   v[ISC_LABEL_OFF_SEC] = offSec;
   v[ISC_LABEL_BLEACH_PROB] = bleachProb;
   v[ISC_LABEL_PHOTON_CV] = photonCV;
   v[ISC_LABEL_INITIAL_ON_SEC] = initialOnSec;
   v[ISC_LABEL_PRE_STATE] = preState ? 1.0 : 0.0;
   v[ISC_LABEL_ORIENT_MODE] = ISC_ORIENT_FREE;
   v[ISC_LABEL_ORIENT_POLAR_DEG] = 90;
   return v;
}

int CellFieldSource::AllStructures() { return (1 << ISC_STRUCT_COUNT) - 1; }

int CellFieldSource::PersistentMask() const
{
   int m = 0;
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
      if (settings_.LabelMode(s) == ISC_MODE_DNA_PAINT)
         m |= 1 << s;
   return m;
}

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
      // In the browser (where the movie world now lives across movies) a
      // 4M soft cap: a 256 px WideField movie uses ~4M dyes, which the cap
      // never evicts while they are in use.
#if defined(__EMSCRIPTEN__)
      isc_world_set_dye_cache(w, 4e6);
#else
      isc_world_set_dye_cache(w, 16e6);
#endif
      isc_world_free(world_);
      world_ = w;
      settings_ = s;
      settings_.labels.assign(1, std::vector<double>{ -1.0 }); // force the labels below
      cacheDirApplied_ = false;
   }
   if (!settings_.SameLabels(s))
   {
      if (s.labels.size() > static_cast<size_t>(ISC_STRUCT_COUNT))
      {
         err = "more labels than structures";
         return false;
      }
      for (int st = 0; st < ISC_STRUCT_COUNT; ++st)
      {
         const std::vector<double> none;
         const std::vector<double>& v = static_cast<size_t>(st) < s.labels.size() ? s.labels[static_cast<size_t>(st)] : none;
         const int r = isc_world_set_label(world_, st, v.empty() ? nullptr : v.data(), static_cast<int32_t>(v.size()));
         if (r == -2)
         {
            err = "SPT motion and off-target binding are not implemented yet";
            return false;
         }
         if (r != 0)
         {
            err = "invalid dye label (density and fraction 0..1, rates >= 0, ON time > 0, a pre state only for PALM)";
            return false;
         }
      }
      settings_.labels = s.labels;
   }
   if (!cacheDirApplied_ || settings_.cacheDir != s.cacheDir)
   {
      // The packed-block store (a cache only); an unusable directory means
      // no store, not an error.
      isc_world_set_cache_dir(world_, s.cacheDir.c_str());
      settings_.cacheDir = s.cacheDir;
      cacheDirApplied_ = true;
   }
   return true;
}

namespace {
// The rows of a cap/total query (stride ISC_EVENT_STRIDE) as BlinkEvents in q's frame.
template <class Query>
bool EventRows(const CellFieldQuery& q, std::vector<double>& buf, Query query, std::vector<BlinkEvent>& out)
{
   // The core's cap/total convention: grow and ask again (the second call is
   // served from its caches).
   int32_t cap = static_cast<int32_t>(buf.size() / ISC_EVENT_STRIDE);
   int32_t n;
   for (;;)
   {
      n = query(buf.empty() ? nullptr : buf.data(), cap);
      if (n < 0)
         return false;
      if (n <= cap)
         break;
      cap = n + n / 4;
      buf.resize(static_cast<size_t>(cap) * ISC_EVENT_STRIDE);
   }
   out.reserve(out.size() + static_cast<size_t>(n));
   for (int32_t i = 0; i < n; ++i)
   {
      const double* e = &buf[static_cast<size_t>(i) * ISC_EVENT_STRIDE];
      BlinkEvent b;
      b.xUm = e[0] - q.originXUm;
      b.yUm = e[1] - q.originYUm;
      b.zNm = (e[2] - q.zRefUm) * 1000.0;
      b.tStart = q.frameIndex + (e[3] - q.tSec) / q.frameSec;
      b.tEnd = q.frameIndex + (e[4] - q.tSec) / q.frameSec;
      b.brightness = e[5];
      b.structure = static_cast<int>(e[7]);
      b.state = static_cast<int>(e[8]);
      b.aux = e[9];
      out.push_back(b);
   }
   return true;
}
} // namespace

bool CellFieldSource::Events(const CellFieldQuery& q, std::vector<BlinkEvent>& out)
{
   if (!world_ || !(q.frameSec > 0))
      return false;
   const double inf = std::numeric_limits<double>::infinity();
   const double zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -inf;
   const double zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : inf;
   const double t1 = q.tSec + q.spanSec;
   return EventRows(q, buf_, [&](double* b, int32_t cap) {
      return isc_events_in_window(world_, q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec, t1, b, cap);
   }, out);
}

bool CellFieldSource::Continuous(const CellFieldQuery& q, std::vector<BlinkEvent>& out)
{
   if (!world_ || !(q.frameSec > 0))
      return false;
   const double inf = std::numeric_limits<double>::infinity();
   const double zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -inf;
   const double zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : inf;
   return EventRows(q, buf_, [&](double* b, int32_t cap) {
      return isc_continuous_in_window(world_, q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec, b, cap);
   }, out);
}

bool CellFieldSource::SetKineticsHistory(const double* rows, int nSeg)
{
   if (!world_)
      return nSeg == 0;
   return isc_world_set_kinetics_history(world_, nSeg > 0 ? rows : nullptr, static_cast<int32_t>(std::max(0, nSeg))) == 0;
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
                                int ny, int nz, int structureMask, float* out)
{
   if (!world_)
      return -1;
   return isc_density3d_in_window(world_, x0, y0, x1, y1, zMin, zMax, nx, ny, nz, structureMask, out);
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
