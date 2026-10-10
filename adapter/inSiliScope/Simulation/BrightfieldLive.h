///////////////////////////////////////////////////////////////////////////////
// FILE:          BrightfieldLive.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Live BrightField: the scene of the current pose, a cache of
//                its images at recent foci, and a thread that computes the
//                foci the user is likely to go to next (spec/PERF_PASS.md
//                phase 4: at 20x a focus change re-images a large grid,
//                0.3-0.8 s). The images are Image()'s pixels; the prefetch
//                computes on half the cores under a shared lock and stops
//                before its next source when the scene is rebuilt or the
//                producer needs an image it does not have.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "BrightfieldRender.h"
#include "Parallel.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sim {

class BrightfieldLive
{
public:
   BrightfieldLive() = default;
   BrightfieldLive(const BrightfieldLive&) = delete;
   BrightfieldLive& operator=(const BrightfieldLive&) = delete;
   ~BrightfieldLive() { Reset(); }

   // The transmitted intensity at focusUm of the scene for spec and world
   // (rebuilt when they changed: the prefetch stops first). A cached focus
   // (the prefetch's, or one shown before) costs a copy; the focus being
   // prefetched is waited for.
   bool Image(CellFieldSource& src, const BrightfieldSpec& spec, uint64_t worldVersion, double focusUm,
              std::vector<float>& out, std::string& err);
   // The foci to compute in the background, most wanted first (replaces the
   // previous list; those cached are skipped).
   void Prefetch(const std::vector<double>& foci);
   // Drops the foci to come and stops the one in flight (the scene and the
   // cache stay).
   void StopPrefetch();
   // Stops the thread and frees the scene and the cache.
   void Reset();

private:
   void Loop();
   // The cached image within kFocusTolUm of focusUm (hold m_); touch: moved
   // to the front (it was used).
   const std::vector<float>* Find(double focusUm, bool touch = true);
   void Store(double focusUm, std::vector<float>&& img);   // hold m_

   static constexpr double kFocusTolUm = 1e-6;   // a focus reached by other arithmetic (z + step) is the same plane
   BrightfieldScene scene_;
   std::shared_mutex sceneMutex_;   // shared: computing an image; exclusive: rebuilding
   std::mutex m_;
   std::condition_variable cv_;
   std::deque<std::pair<double, std::vector<float>>> cache_;   // newest first, of the scene version cacheVersion_
   uint64_t cacheVersion_ = 0;
   size_t cacheCap_ = 8;
   std::vector<double> wish_;
   bool inFlight_ = false;
   double inFlightFocus_ = 0.0;
   bool run_ = false, updating_ = false;
   std::atomic<unsigned> cancelGen_{0};
   std::thread thread_;
   // The prefetch's threads: half the cores, so a live frame's own work (the
   // shared pool) keeps the other half (below normal priority instead, the
   // prefetch starved behind background load and missed most focus steps).
   std::unique_ptr<ParallelPool> pool_;
};

} // namespace sim
