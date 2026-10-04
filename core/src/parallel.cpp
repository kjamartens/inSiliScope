#include "parallel.h"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define ISC_NO_THREADS 1
#endif

#include <algorithm>
#include <atomic>
#ifndef ISC_NO_THREADS
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#endif

namespace isc {

namespace {
std::atomic<int> g_want{0};
#ifndef ISC_NO_THREADS
std::atomic<bool> g_busy{false};
thread_local int t_depth = 0;
thread_local WorkerPool* t_pool = nullptr;
#endif
} // namespace

void SetWorldThreads(int n) { g_want.store(std::max(0, n)); }

int WorldThreads()
{
#ifdef ISC_NO_THREADS
   return 1;
#else
   const int want = g_want.load();
   if (want > 0) return want;
   return (int)std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
#endif
}

#ifndef ISC_NO_THREADS

// Sleeping workers; one job at a time (Run holds job_). A job is a
// function, an item counter the workers and the caller claim from, and the
// number of workers that take part (the caller's helpers).
class WorkerPool {
public:
   explicit WorkerPool(int workers)
   {
      workers_.reserve((size_t)std::max(0, workers));
      for (int i = 0; i < workers; i++) workers_.emplace_back([this, i] { Loop(i); });
   }
   ~WorkerPool()
   {
      {
         std::lock_guard<std::mutex> lk(m_);
         quit_ = true;
      }
      cv_.notify_all();
      for (std::thread& t : workers_) t.join();
   }
   size_t Workers() const { return workers_.size(); }

   // False (nothing run) if another Run is in progress on this pool.
   bool Run(size_t n, size_t helpers, const std::function<void(size_t)>& fn)
   {
      if (!job_.try_lock()) return false;
      {
         std::lock_guard<std::mutex> lk(m_);
         fn_ = &fn;
         n_ = n;
         next_.store(0);
         helpers_ = std::min(helpers, workers_.size());
         pending_.store((int)helpers_);
         ++generation_;
      }
      cv_.notify_all();
      Work();
      {
         std::unique_lock<std::mutex> lk(m_);
         done_.wait(lk, [this] { return pending_.load() == 0; });
         fn_ = nullptr;
      }
      job_.unlock();
      return true;
   }

private:
   void Work()
   {
      for (size_t i; (i = next_.fetch_add(1)) < n_;) (*fn_)(i);
   }
   void Loop(int index)
   {
      uint64_t seen = 0;
      for (;;) {
         {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
            if ((size_t)index >= helpers_) continue;   // not this job's helper
         }
         t_depth++;
         Work();
         t_depth--;
         if (pending_.fetch_sub(1) == 1) {
            std::lock_guard<std::mutex> lk(m_);
            done_.notify_all();
         }
      }
   }

   std::vector<std::thread> workers_;
   std::mutex m_, job_;
   std::condition_variable cv_, done_;
   bool quit_ = false;
   uint64_t generation_ = 0;
   const std::function<void(size_t)>* fn_ = nullptr;
   size_t n_ = 0, helpers_ = 0;
   std::atomic<size_t> next_{0};
   std::atomic<int> pending_{0};
};

namespace {
std::mutex g_poolMutex;
std::weak_ptr<WorkerPool> g_pool;
} // namespace

std::shared_ptr<WorkerPool> AcquireWorkerPool()
{
   const int workers = WorldThreads() - 1;
   if (workers <= 0) return nullptr;
   std::lock_guard<std::mutex> lk(g_poolMutex);
   std::shared_ptr<WorkerPool> p = g_pool.lock();
   if (!p) {
      p = std::make_shared<WorkerPool>(workers);
      g_pool = p;
   }
   return p;
}

PoolScope::PoolScope(const std::shared_ptr<WorkerPool>& pool) : prev_(t_pool) { t_pool = pool.get(); }
PoolScope::~PoolScope() { t_pool = prev_; }

#else

class WorkerPool {};
std::shared_ptr<WorkerPool> AcquireWorkerPool() { return nullptr; }
PoolScope::PoolScope(const std::shared_ptr<WorkerPool>&) : prev_(nullptr) {}
PoolScope::~PoolScope() { (void)prev_; }

#endif

void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn)
{
#ifndef ISC_NO_THREADS
   const size_t perThread = std::max<size_t>(1, grain);
   const size_t helpers = std::min((size_t)WorldThreads() - 1, n / perThread > 0 ? n / perThread - 1 : 0);
   if (helpers > 0 && t_depth == 0) {
      if (t_pool) {
         // A nested or concurrent Run (another thread's world on the same
         // pool) finds it busy and falls through to the serial loop.
         if (t_pool->Run(n, helpers, fn)) return;
      } else {
         bool expected = false;
         if (g_busy.compare_exchange_strong(expected, true)) {
            std::atomic<size_t> next{0};
            auto work = [&]() {
               t_depth++;
               for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
               t_depth--;
            };
            std::vector<std::thread> pool;
            pool.reserve(helpers);
            for (size_t t = 0; t < helpers; t++) pool.emplace_back(work);
            work();
            for (std::thread& t : pool) t.join();
            g_busy.store(false);
            return;
         }
      }
   }
#else
   (void)grain;
#endif
   for (size_t i = 0; i < n; i++) fn(i);
}

} // namespace isc
