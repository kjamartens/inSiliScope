// ParallelFor for the world's independent per-item work (packing blocks and
// their relaxation, cell assets and microtubules, dye blocks, blink
// schedules). Every item is a pure function of its own address and writes
// only its own slot, and the callers keep their serial order when they use
// the results, so nothing depends on the thread count or on scheduling.
//
// Threads: a World holds a shared WorkerPool (sleeping workers, created on
// first use) and installs it for the calls it makes (PoolScope), so a
// ParallelFor costs a wake-up instead of a thread start; the pool is joined
// when the last World holding it is freed, so a host DLL that frees its
// worlds can still unload at any time. Without a scope (isc_pack_window,
// tests) threads are made per call as before. Serial without threads (WASM
// without pthreads), inside another ParallelFor, and while another
// thread's ParallelFor runs (two worlds on two threads never oversubscribe).
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace isc {

// Runs fn(i) for i in [0, n) on the caller plus up to WorldThreads() - 1
// threads, at least `grain` items per thread. fn must not throw.
void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn);

// Threads ParallelFor may use: 0 = the hardware's, capped at 16 (the
// default); 1 = serial. A pool made earlier keeps its worker count; the
// value caps how many of them a call uses.
void SetWorldThreads(int n);
int WorldThreads();

class WorkerPool;
// The process's shared pool (made on the first call, with WorldThreads() - 1
// workers; nullptr without threads). Hold it as long as ParallelFor should
// use it; the pool is destroyed (its workers joined) when the last holder
// lets go.
std::shared_ptr<WorkerPool> AcquireWorkerPool();

// While alive, ParallelFor on this thread runs on `pool` (nullptr: as
// without a scope). Nests: the previous scope is restored.
class PoolScope {
public:
   explicit PoolScope(const std::shared_ptr<WorkerPool>& pool);
   ~PoolScope();
   PoolScope(const PoolScope&) = delete;
   PoolScope& operator=(const PoolScope&) = delete;
private:
   WorkerPool* prev_;
};

} // namespace isc
