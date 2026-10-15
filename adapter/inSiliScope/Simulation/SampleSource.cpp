///////////////////////////////////////////////////////////////////////////////
// FILE:          SampleSource.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See SampleSource.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "SampleSource.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace sim {

int SampleSettings::LabelMode(int s) const
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
   for (int i = ISC_LABEL_BINDER_NM; i <= ISC_LABEL_LINKER_MAX_NM; ++i)
      v[static_cast<size_t>(i)] = std::numeric_limits<double>::quiet_NaN();   // the structure's built-in linkage
   return v;
}

int SampleSource::AllStructures() { return (1 << ISC_STRUCT_COUNT) - 1; }

int SampleSource::PersistentMask() const
{
   int m = 0;
   for (int s = 0; s < ISC_STRUCT_COUNT; ++s)
      if (settings_.LabelMode(s) == ISC_MODE_DNA_PAINT)
         m |= 1 << s;
   return m;
}

SampleSource::~SampleSource()
{
   isc_world_free(world_);
}

bool SampleSource::Configure(const SampleSettings& s, std::string& err)
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
      double heights[2] = { -1, -1 };
      bool origami = false;
      for (const auto& kv : s.params)
         origami = origami || (kv.first == "specimen" && static_cast<int>(kv.second) == ISC_SPECIMEN_NANORULERS);
      if (origami)
         isc_origami_heights(p, heights);
      origamiTopUm_ = origami ? heights[1] : -1;
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
bool EventRows(const SampleQuery& q, std::vector<double>& buf, Query query, std::vector<BlinkEvent>& out)
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
      BlinkEvent b;
      SampleSource::MapRow(q, &buf[static_cast<size_t>(i) * ISC_EVENT_STRIDE], b);
      out.push_back(b);
   }
   return true;
}
} // namespace

void SampleSource::MapRow(const SampleQuery& q, const double* e, BlinkEvent& b)
{
   b.xUm = e[0] - q.originXUm;
   b.yUm = e[1] - q.originYUm;
   b.zNm = (e[2] - q.zRefUm) * 1000.0;
   b.tStart = q.frameIndex + (e[3] - q.tSec) / q.frameSec;
   b.tEnd = q.frameIndex + (e[4] - q.tSec) / q.frameSec;
   b.brightness = e[5];
   b.structure = static_cast<int>(e[7]);
   b.state = static_cast<int>(e[8]);
   b.aux = e[9];
}

bool SampleSource::ContinuousRows(double x0, double y0, double x1, double y1, double zMin, double zMax, double tMin,
                                  std::vector<double>& rows)
{
   if (!world_)
      return false;
   int32_t cap = static_cast<int32_t>(rows.size() / ISC_EVENT_STRIDE);
   for (;;)
   {
      const int32_t n = isc_continuous_in_window(world_, x0, y0, x1, y1, zMin, zMax, tMin,
                                                 rows.empty() ? nullptr : rows.data(), cap);
      if (n < 0)
         return false;
      if (n <= cap)
      {
         rows.resize(static_cast<size_t>(n) * ISC_EVENT_STRIDE);
         return true;
      }
      cap = n + n / 4;
      rows.resize(static_cast<size_t>(cap) * ISC_EVENT_STRIDE);
   }
}

bool SampleSource::Events(const SampleQuery& q, std::vector<BlinkEvent>& out)
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

bool SampleSource::Continuous(const SampleQuery& q, std::vector<BlinkEvent>& out)
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

bool SampleSource::Sites(double x0, double y0, double x1, double y1, double zMin, double zMax,
                         std::vector<double>& out)
{
   if (!world_)
      return false;
   int32_t cap = static_cast<int32_t>(out.size() / ISC_SITE_STRIDE);
   for (;;)
   {
      const int32_t n = isc_sites_in_window(world_, x0, y0, x1, y1, zMin, zMax, out.empty() ? nullptr : out.data(), cap);
      if (n < 0)
         return false;
      if (n <= cap)
      {
         out.resize(static_cast<size_t>(n) * ISC_SITE_STRIDE);
         return true;
      }
      cap = n + n / 4;
      out.resize(static_cast<size_t>(cap) * ISC_SITE_STRIDE);
   }
}

bool SampleSource::SetKineticsHistory(const double* rows, int nSeg)
{
   if (!world_)
      return nSeg == 0;
   return isc_world_set_kinetics_history(world_, nSeg > 0 ? rows : nullptr, static_cast<int32_t>(std::max(0, nSeg))) == 0;
}

bool SampleSource::SetClockMap(const std::vector<double>* buffer)
{
   const bool none = !buffer || buffer->empty();
   if (!world_)
      return none;
   return isc_world_set_clock_map(world_, none ? nullptr : buffer->data(),
                                  none ? 0 : static_cast<int32_t>(buffer->size())) == 0;
}

bool SampleSource::EventsInMap(const SampleQuery& q, std::vector<BlinkEvent>& out)
{
   if (!world_ || !(q.frameSec > 0))
      return false;
   const double inf = std::numeric_limits<double>::infinity();
   const double zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -inf;
   const double zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : inf;
   const double t1 = q.tSec + q.spanSec;
   return EventRows(q, buf_, [&](double* b, int32_t cap) {
      return isc_events_in_map(world_, q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec, t1, b, cap);
   }, out);
}

bool SampleSource::ContinuousInMap(const SampleQuery& q, std::vector<BlinkEvent>& out, std::vector<double>* clockRate)
{
   if (!world_ || !(q.frameSec > 0))
      return false;
   const double inf = std::numeric_limits<double>::infinity();
   const double zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -inf;
   const double zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : inf;
   int32_t cap = static_cast<int32_t>(buf_.size() / ISC_MAP_WINDOW_STRIDE);
   int32_t n;
   for (;;)
   {
      n = isc_continuous_in_map(world_, q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec,
                                buf_.empty() ? nullptr : buf_.data(), cap);
      if (n < 0)
         return false;
      if (n <= cap)
         break;
      cap = n + n / 4;
      buf_.resize(static_cast<size_t>(cap) * ISC_MAP_WINDOW_STRIDE);
   }
   out.reserve(out.size() + static_cast<size_t>(n));
   if (clockRate)
      clockRate->reserve(clockRate->size() + 2 * static_cast<size_t>(n));
   for (int32_t i = 0; i < n; ++i)
   {
      const double* row = &buf_[static_cast<size_t>(i) * ISC_MAP_WINDOW_STRIDE];
      BlinkEvent b;
      MapRow(q, row, b);
      out.push_back(b);
      if (clockRate)
      {
         clockRate->push_back(row[ISC_EVENT_STRIDE]);
         clockRate->push_back(row[ISC_EVENT_STRIDE + 1]);
      }
   }
   return true;
}

bool SampleSource::Prefetch(const SampleQuery& q, double marginUm, double budgetMs, bool inMap)
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
      const int32_t done =
         inMap ? isc_world_prefetch_map(world_, q.x0Um - m, q.y0Um - m, q.x1Um + m, q.y1Um + m, -inf, inf, q.tSec,
                                        q.tSec + q.spanSec, left)
               : isc_world_prefetch(world_, q.x0Um - m, q.y0Um - m, q.x1Um + m, q.y1Um + m, -inf, inf, q.tSec,
                                    q.tSec + q.spanSec, left);
      if (done != 1)
         return false;
   }
   return true;
}

long SampleSource::Density3d(double x0, double y0, double x1, double y1, double zMin, double zMax, int nx,
                                int ny, int nz, int structureMask, float* out)
{
   if (!world_)
      return -1;
   return isc_density3d_in_window(world_, x0, y0, x1, y1, zMin, zMax, nx, ny, nz, structureMask, out);
}

long SampleSource::OpticalVolume(double x0, double y0, double x1, double y1, double zMin, double zMax, int nx,
                                    int ny, int nz, int sub, float* out)
{
   if (!world_)
      return -1;
   return isc_optical_volume_in_window(world_, x0, y0, x1, y1, zMin, zMax, nx, ny, nz, sub, out);
}

double SampleSource::MaxCellHeight(double x0, double y0, double x1, double y1)
{
   if (!world_)
      return -1;
   if (origamiTopUm_ >= 0)
   {
      const int32_t n = isc_origami_in_window(world_, x0, y0, x1, y1, nullptr, 0);
      return n < 0 ? -1 : n > 0 ? origamiTopUm_ : 0.0;
   }
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
