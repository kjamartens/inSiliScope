// ctest live_scope: live imaging of one sample (Simulation/LiveScope.h, the viewer's isc_live_*):
//   - the FOV centre's clock grows by the exposure per lit frame, and not with the lasers off (or countLight 0);
//   - another place is fresh (clock 0), coming back finds the light it had;
//   - another world (world-seed) is a fresh sample: time and clocks back to 0;
//   - two sessions with the same requests give the same frames, bit for bit; the noise differs frame to frame;
//   - dSTORM's initial ON fades where the light has been and not elsewhere.
//
//   live_scope_check     exit code 0 = all checks passed
#include "LiveScope.h"
#include "ScopeMovie.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok) g_failures++;
}

ScopeSpec Spec(const std::string& extra)
{
   ScopeSpec s;
   std::string err;
   const std::string text = "size=32 exposure-ms=100 npc-dye=None mt-mode=dSTORM x=3 y=-2 z=0.5 " + extra;
   if (!ParseScopeSpec(text, s, err))
   {
      std::printf("FAIL  spec: %s\n", err.c_str());
      g_failures++;
   }
   return s;
}

double Mean(const std::vector<uint16_t>& a)
{
   double s = 0;
   for (uint16_t v : a) s += v;
   return a.empty() ? 0 : s / a.size();
}

struct Shot
{
   std::vector<uint16_t> adu;
   LiveFrameInfo info;
   bool ok = false;
};

Shot Take(LiveScope& live, const ScopeSpec& spec, bool count = true)
{
   Shot s;
   std::string err;
   s.ok = live.Frame(spec, count, s.adu, s.info, err);
   if (!s.ok) std::printf("      (frame failed: %s)\n", err.c_str());
   return s;
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   const ScopeSpec here = Spec(""), away = Spec("x=40"), dark = Spec("laser-561=0 laser-640=0 light-preset=-1");

   LiveScope live;
   std::vector<Shot> shots;
   for (int k = 0; k < 6; k++)
      shots.push_back(Take(live, here));
   bool ok = true;
   for (int k = 0; k < 6; k++)
      ok = ok && shots[k].ok && shots[k].info.lit && std::fabs(shots[k].info.litSec - 0.1 * k) < 1e-6 &&
           std::fabs(shots[k].info.tSec - 0.1 * k) < 1e-9;
   Check(ok, "the FOV centre's clock and the time grow by the exposure per lit frame (0.0 .. 0.5 s)");
   const Shot d1 = Take(live, dark), d2 = Take(live, dark);
   Check(d1.adu != d2.adu, "the noise differs frame to frame (two dark frames)");
   Check(d1.ok && d2.ok && !d1.info.lit && std::fabs(d1.info.litSec - 0.6) < 1e-6 && std::fabs(d2.info.litSec - 0.6) < 1e-6,
         "lasers off: the frame lights nothing (the clock stays at 0.6 s)");
   const Shot uncounted = Take(live, here, false), after = Take(live, here);
   Check(uncounted.ok && !uncounted.info.lit && std::fabs(after.info.litSec - 0.6) < 1e-6,
         "countLight 0: the frame's light is not added");

   const Shot a = Take(live, away), back = Take(live, here);
   Check(a.ok && a.info.litSec == 0.0 && back.ok && std::fabs(back.info.litSec - 0.7) < 1e-6,
         "40 um away is fresh (clock 0); back at the first place: 0.7 s");

   // The initial ON (dSTORM): bright at a fresh place, fading under light.
   LiveScope fade;
   const ScopeSpec bright = Spec("laser-640=5");
   const Shot first = Take(fade, bright);
   Shot last;
   for (int k = 0; k < 80; k++)
      last = Take(fade, bright);
   const Shot fresh = Take(fade, Spec("x=-30 laser-640=5"));
   char msg[200];
   std::snprintf(msg, sizeof msg, "dSTORM initial ON fades under light: mean %.0f ADU at 0 s, %.0f after %.1f s; 33 um away fresh (clock %.1f s)",
                 Mean(first.adu), Mean(last.adu), last.info.litSec, fresh.info.litSec);
   Check(first.ok && last.ok && Mean(last.adu) < 0.5 * Mean(first.adu) && fresh.info.litSec == 0.0, msg);

   // Another world: a fresh sample.
   const Shot other = Take(live, Spec("world-seed=7"));
   Check(other.ok && other.info.tSec == 0.0 && other.info.litSec == 0.0, "another world (world-seed) is a fresh sample");

   // Determinism: the same requests on a second session.
   LiveScope again;
   bool same = true;
   for (int k = 0; k < 6; k++)
   {
      const Shot s = Take(again, here);
      same = same && s.ok && s.adu == shots[k].adu;
   }
   Check(same, "a second session with the same requests: the same frames, bit for bit");

   std::printf("live_scope: %d failure(s), %.1f s\n", g_failures,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
