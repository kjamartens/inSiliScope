#include "parallel.h"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define ISC_NO_THREADS 1
#endif

#include <algorithm>
#include <atomic>
#ifndef ISC_NO_THREADS
#include <thread>
#include <vector>
#endif

namespace isc {

namespace {
std::atomic<int> g_want{0};
#ifndef ISC_NO_THREADS
std::atomic<bool> g_busy{false};
thread_local int t_depth = 0;
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

void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn)
{
#ifndef ISC_NO_THREADS
   const size_t perThread = std::max<size_t>(1, grain);
   const size_t helpers = std::min((size_t)WorldThreads() - 1, n / perThread > 0 ? n / perThread - 1 : 0);
   bool expected = false;
   if (helpers > 0 && t_depth == 0 && g_busy.compare_exchange_strong(expected, true)) {
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
#else
   (void)grain;
#endif
   for (size_t i = 0; i < n; i++) fn(i);
}

} // namespace isc
