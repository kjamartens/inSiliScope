///////////////////////////////////////////////////////////////////////////////
// FILE:          Timing.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Opt-in phase timing for the movie paths (cli, viewer, the
//                checks): set the environment variable ISC_TIMING=1 and the
//                phases of a movie (world, PSF, query, render, noise, ...)
//                are printed to stderr as "ISC_TIMING <phase> <seconds>".
//                Off by default and never on under Emscripten unless the
//                page sets Module.ENV. Output of the renders is unaffected.
//                Log only from the calling thread, at phase boundaries.
//                A host can also collect the phases in memory (TimingCollect:
//                count, total and longest per phase name, taken as JSON by
//                TimingProfileTake; the adapter's Test_Profile, read by
//                tools/bench_live.py --profile), with or without the printing.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace sim {

using TimingClock = std::chrono::steady_clock;

// Set from a host without an environment (the viewer's WASM: isc_timing).
inline bool& TimingForced()
{
   static bool forced = false;
   return forced;
}
inline bool TimingPrinting()
{
   static const bool env = [] {
      const char* e = std::getenv("ISC_TIMING");
      return e && *e && *e != '0';
   }();
   return env || TimingForced();
}
// In-memory collection of the phases (process-wide; any thread).
// Starts on with the environment variable ISC_PROFILE=1 (from process start:
// a configuration's load and first image are profiled too).
inline std::atomic<bool>& TimingCollect()
{
   static std::atomic<bool> on{[] {
      const char* e = std::getenv("ISC_PROFILE");
      return e && *e && *e != '0';
   }()};
   return on;
}
inline bool TimingEnabled()
{
   return TimingPrinting() || TimingCollect().load(std::memory_order_relaxed);
}

struct TimingStat
{
   long count = 0;
   double total = 0.0, longest = 0.0;   // seconds
};
struct TimingProfile
{
   std::mutex mutex;
   std::map<std::string, TimingStat> phases;
   TimingClock::time_point since = TimingClock::now();
};
inline TimingProfile& SharedTimingProfile()
{
   static TimingProfile p;
   return p;
}
inline void TimingProfileAdd(const char* phase, double seconds)
{
   TimingProfile& p = SharedTimingProfile();
   std::lock_guard<std::mutex> g(p.mutex);
   TimingStat& st = p.phases[phase];
   ++st.count;
   st.total += seconds;
   if (seconds > st.longest)
      st.longest = seconds;
}
// {"seconds": <wall time since the last take>, "phases": {"<name>": {"count": n, "totalMs": t, "meanMs": m,
// "maxMs": x}, ...}}; then starts afresh.
inline std::string TimingProfileTake()
{
   TimingProfile& p = SharedTimingProfile();
   std::lock_guard<std::mutex> g(p.mutex);
   const auto now = TimingClock::now();
   char b[256];
   std::snprintf(b, sizeof b, "{\"seconds\": %.4f, \"phases\": {",
                 std::chrono::duration<double>(now - p.since).count());
   std::string j = b;
   bool first = true;
   for (const auto& kv : p.phases)
   {
      const TimingStat& st = kv.second;
      std::snprintf(b, sizeof b, "%s\"%s\": {\"count\": %ld, \"totalMs\": %.4f, \"meanMs\": %.4f, \"maxMs\": %.4f}",
                    first ? "" : ", ", kv.first.c_str(), st.count, 1000.0 * st.total,
                    st.count ? 1000.0 * st.total / st.count : 0.0, 1000.0 * st.longest);
      j += b;
      first = false;
   }
   j += "}}";
   p.phases.clear();
   p.since = now;
   return j;
}

inline double TimingSince(TimingClock::time_point t0)
{
   return std::chrono::duration<double>(TimingClock::now() - t0).count();
}

// One line per phase; `extra` (optional) is appended as is.
inline void TimingLog(const char* phase, double seconds, const char* extra = nullptr)
{
   if (TimingCollect().load(std::memory_order_relaxed))
      TimingProfileAdd(phase, seconds);
   if (!TimingPrinting())
      return;
   if (extra && *extra)
      std::fprintf(stderr, "ISC_TIMING %-28s %9.4f s  %s\n", phase, seconds, extra);
   else
      std::fprintf(stderr, "ISC_TIMING %-28s %9.4f s\n", phase, seconds);
   std::fflush(stderr);
}

// Logs the time from its construction to its end as one phase.
struct TimingScope
{
   const char* phase;
   TimingClock::time_point t0 = TimingClock::now();
   explicit TimingScope(const char* p) : phase(p) {}
   ~TimingScope() { TimingLog(phase, TimingSince(t0)); }
   TimingScope(const TimingScope&) = delete;
   TimingScope& operator=(const TimingScope&) = delete;
};

// Accumulates the wall time of repeated phases (per-frame work) for one
// summary line at the end.
struct TimingSum
{
   double seconds = 0.0;
   long count = 0;
   TimingClock::time_point t0;
   void Start() { if (TimingEnabled()) t0 = TimingClock::now(); }
   void Stop()
   {
      if (!TimingEnabled())
         return;
      seconds += TimingSince(t0);
      ++count;
   }
   void Log(const char* phase) const
   {
      if (!TimingEnabled())
         return;
      char b[64];
      std::snprintf(b, sizeof b, "(%ld x %.3f ms)", count, count ? 1000.0 * seconds / count : 0.0);
      TimingLog(phase, seconds, b);
   }
};

} // namespace sim
