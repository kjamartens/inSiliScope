// ctest emitter_density: General_EmitterDensityPerSec means blink onsets per
// um^2 per second, whatever the exposure, ON lifetime or bleaching, in both
// the precomputed (GenerateAllEvents) and the live (AdvanceOneFrame) path.
// The conversion mirrors the camera's SnapshotParams().
#include "SMLMPatterns.h"
#include "SMLMSimulation.h"
#include "SMLMStructures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace sim;

namespace {

double Measure(double densPerSec, double expSec, double onSec, double bleach, bool live, double seconds)
{
   SimulationParams p;
   p.onLifetimeFrames = std::min(onSec / expSec, 20000.0);
   p.emitterDensity = densPerSec * expSec * std::max(p.onLifetimeFrames, 0.01);   // SnapshotParams
   p.blinkBleachProb = bleach;
   p.offLifetimeFrames = 1.0 / expSec;   // 1 s dark time
   const double W = 12.8;
   EmitterModel m;
   StructureParams sp;
   sp.zRangeNm = 0;
   m.SetPattern(CreatePattern(PATTERN_RANDOM, "", DefaultResolutionSpacingsNm(), sp, W, W, 1));
   std::mt19937_64 rng(5);
   const long N = static_cast<long>(seconds / expSec);
   double onsets = 0;
   if (!live)
   {
      for (const BlinkEvent& e : m.GenerateAllEvents(N, W, W, p, rng))
         if (e.tStart >= 0 && e.tStart < N) onsets++;
   }
   else
   {
      m.ResetLive(W, W);
      for (long f = 0; f < N; f++)
         for (const BlinkEvent& e : m.AdvanceOneFrame(f, W, W, p, rng))
            if (e.tStart >= f && e.tStart < f + 1) onsets++;
   }
   return onsets / (W * W) / (N * expSec);
}

} // namespace

int main()
{
   int bad = 0;
   for (double dens : { 0.5, 5.0 })
      for (double expSec : { 0.01, 0.05 })
         for (double onSec : { 0.02, 0.2 })
            for (double bleach : { 1.0, 0.3 })
               for (bool live : { false, true })
               {
                  const double got = Measure(dens, expSec, onSec, bleach, live, dens < 1 ? 120.0 : 40.0);
                  const bool ok = std::fabs(got / dens - 1) < 0.06;
                  std::printf("%s density %.1f/um2/s, exposure %2.0f ms, ON %.2f s, bleach %.1f, %-11s -> %.3f\n",
                              ok ? "ok  " : "FAIL", dens, expSec * 1000, onSec, bleach,
                              live ? "live" : "precomputed", got);
                  bad += !ok;
               }
   std::printf(bad ? "%d case(s) off by more than 6%%\n" : "emitter density = General_EmitterDensityPerSec in every case\n", bad);
   return bad ? 1 : 0;
}
