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
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <algorithm>
#include <atomic>
#include <vector>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace sim {

inline int& ParallelDepth()
{
   thread_local int depth = 0;
   return depth;
}

// Number of ParallelFor calls that spawned threads (ISC_TIMING summaries).
inline std::atomic<unsigned long>& ParallelForSpawns()
{
   static std::atomic<unsigned long> n{0};
   return n;
}

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
