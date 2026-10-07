///////////////////////////////////////////////////////////////////////////////
// FILE:          BrightfieldLive.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See BrightfieldLive.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BrightfieldLive.h"
#include "Timing.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kCacheBytes = 96e6;   // the focus images kept (4-24 of them)
} // namespace

const std::vector<float>* BrightfieldLive::Find(double focusUm, bool touch)
{
   for (auto it = cache_.begin(); it != cache_.end(); ++it)
      if (std::fabs(it->first - focusUm) <= kFocusTolUm)
      {
         if (!touch)
            return &it->second;
         if (it != cache_.begin())
         {
            std::pair<double, std::vector<float>> e = std::move(*it);
            cache_.erase(it);
            cache_.push_front(std::move(e));
         }
         return &cache_.front().second;
      }
   return nullptr;
}

void BrightfieldLive::Store(double focusUm, std::vector<float>&& img)
{
   if (Find(focusUm, false))
      return;
   cache_.emplace_front(focusUm, std::move(img));
   while (cache_.size() > cacheCap_)
      cache_.pop_back();
}

bool BrightfieldLive::Image(CellFieldSource& src, const BrightfieldSpec& spec, uint64_t worldVersion, double focusUm,
                            std::vector<float>& out, std::string& err)
{
   // Only this thread changes the scene: reading it here needs no lock.
   if (!scene_.Matches(src, spec, worldVersion))
   {
      {
         std::lock_guard<std::mutex> g(m_);
         updating_ = true;
         wish_.clear();
      }
      ++cancelGen_;
      bool ok;
      {
         std::unique_lock<std::shared_mutex> x(sceneMutex_);   // the prefetch's block in flight ends first
         ok = scene_.Update(src, spec, worldVersion, err);
      }
      std::lock_guard<std::mutex> g(m_);
      updating_ = false;
      cache_.clear();
      cacheVersion_ = scene_.Version();
      const double bytes = static_cast<double>(spec.width) * spec.height * sizeof(float);
      cacheCap_ = static_cast<size_t>(std::min(24.0, std::max(4.0, kCacheBytes / std::max(1.0, bytes))));
      if (!ok)
         return false;
   }
   {
      std::unique_lock<std::mutex> g(m_);
      for (;;)
      {
         if (const std::vector<float>* img = Find(focusUm))
         {
            out = *img;
            TimingLog("bf.focus-cached", 0.0);
            return true;
         }
         // Being prefetched: wait for it (sooner than starting over).
         if (!(inFlight_ && std::fabs(inFlightFocus_ - focusUm) <= kFocusTolUm))
            break;
         sim::TimingScope wait("bf.prefetch-wait");
         cv_.wait(g);
      }
   }
   // Not cached: compute it here, the prefetch's image in flight stopped (the
   // cores are this frame's).
   ++cancelGen_;
   std::vector<float> img;
   {
      std::shared_lock<std::shared_mutex> s(sceneMutex_);
      const auto t0 = TimingClock::now();
      if (!scene_.ComputeImage(focusUm, img))
      {
         err = "BrightField: scene not set up.";
         return false;
      }
      TimingLog("bf.image", TimingSince(t0));
   }
   out = img;
   std::lock_guard<std::mutex> g(m_);
   Store(focusUm, std::move(img));
   cv_.notify_all();
   return true;
}

void BrightfieldLive::Prefetch(const std::vector<double>& foci)
{
   {
      std::lock_guard<std::mutex> g(m_);
      wish_.clear();
      for (double f : foci)
         if (!Find(f, false) && !(inFlight_ && std::fabs(inFlightFocus_ - f) <= kFocusTolUm))
            wish_.push_back(f);
      if (wish_.empty())
         return;
      if (!run_)
      {
         run_ = true;
         if (!pool_)
            pool_.reset(new ParallelPool(nullptr, std::max(1u, std::thread::hardware_concurrency() / 2)));
         thread_ = std::thread(&BrightfieldLive::Loop, this);
      }
   }
   cv_.notify_all();
}

void BrightfieldLive::StopPrefetch()
{
   {
      std::lock_guard<std::mutex> g(m_);
      wish_.clear();
   }
   ++cancelGen_;
}

void BrightfieldLive::Reset()
{
   {
      std::lock_guard<std::mutex> g(m_);
      run_ = false;
      wish_.clear();
   }
   ++cancelGen_;
   cv_.notify_all();
   if (thread_.joinable())
      thread_.join();
   pool_.reset();
   std::lock_guard<std::mutex> g(m_);
   cache_.clear();
   scene_ = BrightfieldScene();
}

void BrightfieldLive::Loop()
{
   std::unique_lock<std::mutex> g(m_);
   for (;;)
   {
      cv_.wait(g, [&] { return !run_ || (!wish_.empty() && !updating_); });
      if (!run_)
         return;
      const double focus = wish_.front();
      wish_.erase(wish_.begin());
      if (Find(focus, false))
         continue;
      inFlight_ = true;
      inFlightFocus_ = focus;
      const unsigned gen = cancelGen_.load();
      g.unlock();
      std::vector<float> img;
      bool ok;
      uint64_t version;
      {
         std::shared_lock<std::shared_mutex> s(sceneMutex_);
         const auto t0 = TimingClock::now();
         ok = scene_.ComputeImage(focus, img, [&] { return cancelGen_.load() != gen; }, pool_.get());
         version = scene_.Version();
         if (ok)
            TimingLog("bf.prefetch-image", TimingSince(t0));
      }
      g.lock();
      inFlight_ = false;
      if (ok && version == cacheVersion_)
         Store(focus, std::move(img));
      cv_.notify_all();
   }
}

} // namespace sim
