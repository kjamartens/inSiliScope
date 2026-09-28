// ParallelFor for the world's independent per-item work (packing blocks,
// cell assets, dye blocks, blink schedules). Every item is a pure function of
// its own address and writes only its own slot, and the callers keep their
// serial order when they use the results, so nothing depends on the thread
// count or on scheduling. Threads are made per call (nothing outlives it, so
// a host DLL can unload at any time). Serial without threads (WASM without
// pthreads), inside another ParallelFor, and while another thread's
// ParallelFor runs (two worlds on two threads never oversubscribe).
#pragma once

#include <cstddef>
#include <functional>

namespace isc {

// Runs fn(i) for i in [0, n) on the caller plus up to WorldThreads() - 1
// threads, at least `grain` items per thread. fn must not throw.
void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn);

// Threads ParallelFor may use: 0 = the hardware's, capped at 16 (the
// default); 1 = serial.
void SetWorldThreads(int n);
int WorldThreads();

} // namespace isc
