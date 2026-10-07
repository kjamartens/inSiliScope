///////////////////////////////////////////////////////////////////////////////
// FILE:          Parallel.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   ParallelFor(n, fn): fn(0..n-1) on up to hardware_concurrency
//                threads (serial under Emscripten). Callers make each index's
//                work independent, so results never depend on the thread
//                count. A ParallelFor inside a ParallelFor worker runs
//                serially (no nested thread pools).
//
//                The threads are a persistent pool (creating and joining a
//                dozen threads per call cost ~1 ms, several times per live
//                frame): workers wait for the next call. A call while the pool is busy
//                (another thread's ParallelFor) gets threads of its own, as
//                before the pool. A host that unloads this code
//                (the Micro-Manager DLL) calls ParallelPoolShutdown() first.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <algorithm>
#include <atomic>
#include <vector>
#if !defined(__EMSCRIPTEN__)
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#endif

namespace sim {

inline int& ParallelDepth()
{
   thread_local int depth = 0;
   return depth;
}

// Number of ParallelFor calls that ran on several threads (ISC_TIMING summaries).
inline std::atomic<unsigned long>& ParallelForSpawns()
{
   static std::atomic<unsigned long> n{0};
   return n;
}

#if !defined(__EMSCRIPTEN__)
class ParallelPool
{
public:
   // Runs fn(0..n-1) on the caller and up to helpers pool threads; false if
   // the pool is busy with another caller's job (nothing was run).
   bool Run(unsigned n, unsigned helpers, const std::function<void(unsigned)>& fn)
   {
      std::unique_lock<std::mutex> call(callMutex_, std::try_to_lock);
      if (!call.owns_lock())
         return false;
      std::atomic<unsigned> next{0};
      {
         std::lock_guard<std::mutex> g(m_);
         if (shutdown_)
            return false;
         while (threads_.size() < helpers)
            threads_.emplace_back(&ParallelPool::Worker, this);
         job_ = &fn;
         jobN_ = n;
         next_ = &next;
         ++gen_;
      }
      cv_.notify_all();
      ++ParallelDepth();
      for (unsigned i; (i = next.fetch_add(1)) < n;)
         fn(i);
      --ParallelDepth();
      // Close the job (a worker waking from now on does not take it) and wait
      // for the workers that took it.
      std::unique_lock<std::mutex> g(m_);
      job_ = nullptr;
      done_.wait(g, [&] { return running_ == 0; });
      return true;
   }

   // Joins the workers (no call may run concurrently); a later Run starts
   // new ones.
   void Shutdown()
   {
      std::lock_guard<std::mutex> call(callMutex_);
      std::vector<std::thread> ts;
      {
         std::lock_guard<std::mutex> g(m_);
         shutdown_ = true;
         ts.swap(threads_);
      }
      cv_.notify_all();
      for (std::thread& t : ts)
         if (t.joinable())
            t.join();
      std::lock_guard<std::mutex> g(m_);
      shutdown_ = false;
   }

private:
   void Worker()
   {
      std::unique_lock<std::mutex> g(m_);
      uint64_t seen = gen_;
      for (;;)
      {
         cv_.wait(g, [&] { return shutdown_ || (job_ && gen_ != seen); });
         if (shutdown_)
            return;
         seen = gen_;
         const std::function<void(unsigned)>* fn = job_;
         const unsigned n = jobN_;
         std::atomic<unsigned>* next = next_;
         ++running_;
         g.unlock();
         ++ParallelDepth();
         for (unsigned i; (i = next->fetch_add(1)) < n;)
            (*fn)(i);
         --ParallelDepth();
         g.lock();
         if (--running_ == 0)
            done_.notify_all();
      }
   }

   std::mutex callMutex_, m_;
   std::condition_variable cv_, done_;
   std::vector<std::thread> threads_;
   const std::function<void(unsigned)>* job_ = nullptr;
   unsigned jobN_ = 0;
   std::atomic<unsigned>* next_ = nullptr;
   uint64_t gen_ = 0;
   unsigned running_ = 0;
   bool shutdown_ = false;
};

// Never destroyed: its threads must not be joined from a static destructor
// (under the Windows loader lock); hosts that unload call ParallelPoolShutdown.
inline ParallelPool& SharedParallelPool()
{
   static ParallelPool* p = new ParallelPool();
   return *p;
}

inline void ParallelPoolShutdown()
{
   SharedParallelPool().Shutdown();
}
#endif

template <class Fn>
void ParallelFor(unsigned n, Fn fn)
{
#if defined(__EMSCRIPTEN__)
   for (unsigned i = 0; i < n; ++i)
      fn(i);
#else
   const unsigned T = ParallelDepth() > 0 ? 1u : std::max(1u, std::min(n, std::thread::hardware_concurrency()));
   if (T <= 1)
   {
      for (unsigned i = 0; i < n; ++i)
         fn(i);
      return;
   }
   ParallelForSpawns().fetch_add(1, std::memory_order_relaxed);
   const std::function<void(unsigned)> f = [&fn](unsigned i) { fn(i); };
   if (SharedParallelPool().Run(n, T - 1, f))
      return;
   // The pool is busy (another thread's call): threads of this call's own.
   std::atomic<unsigned> next{0};
   auto worker = [&]() {
      ++ParallelDepth();
      for (unsigned i; (i = next.fetch_add(1)) < n;)
         fn(i);
      --ParallelDepth();
   };
   std::vector<std::thread> pool;
   for (unsigned t = 1; t < T; ++t)
      pool.emplace_back(worker);
   worker();
   for (std::thread& t : pool)
      t.join();
#endif
}

} // namespace sim
