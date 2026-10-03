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
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace sim {

inline bool TimingEnabled()
{
   static const bool on = [] {
      const char* e = std::getenv("ISC_TIMING");
      return e && *e && *e != '0';
   }();
   return on;
}

using TimingClock = std::chrono::steady_clock;

inline double TimingSince(TimingClock::time_point t0)
{
   return std::chrono::duration<double>(TimingClock::now() - t0).count();
}

// One line per phase; `extra` (optional) is appended as is.
inline void TimingLog(const char* phase, double seconds, const char* extra = nullptr)
{
   if (!TimingEnabled())
      return;
   if (extra && *extra)
      std::fprintf(stderr, "ISC_TIMING %-28s %9.4f s  %s\n", phase, seconds, extra);
   else
      std::fprintf(stderr, "ISC_TIMING %-28s %9.4f s\n", phase, seconds);
   std::fflush(stderr);
}

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
