// ctest live_scope: live imaging of one sample (Simulation/LiveScope.h, the viewer's isc_live_*):
//   - the FOV centre's clock grows by the exposure per lit frame, and not with the lasers off (or countLight 0);
//   - another place is fresh (clock 0), coming back finds the light it had;
//   - another world (world-seed) is a fresh sample: time and clocks back to 0;
//   - two sessions with the same requests give the same frames, bit for bit; the noise differs frame to frame;
//   - dSTORM's initial ON fades where the light has been and not elsewhere;
//   - a recording (Frames: n frames at once) = the same frames one by one (within one electron in a few pixels).
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
   // The frame's render paths (the viewer's info line): the blinks' path and the initial-ON population's.
   std::printf("      paths at 0 s: %s; after %.1f s: %s\n", first.info.paths.c_str(), last.info.litSec,
               last.info.paths.c_str());
   Check(first.info.paths.size() > 2 && first.info.paths.front() == '[' && first.info.paths.back() == ']' &&
         last.info.paths.find("SMLM:") != std::string::npos, "each frame names its render paths (SMLM: ... blinks)");

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

   // A recording (Frames: one movie of n frames on the history's clock) = the same frames taken one by one, after a
   // few live frames; with drift it goes frame by frame anyway. Not bit for bit: the continuous dyes' running images
   // (CarryRunningImages, in the process-wide movie cache that both sessions share) round in another order, which
   // moves a Poisson draw by one electron in a few pixels (also on the frame-by-frame drift path). So: >= 99.5 % of
   // the pixels identical, the rest within one electron (4 ADU at 0.25 e-/ADU), the next live frame bit for bit.
   for (const char* mode : { "", "mt-mode=DNA-PAINT", "drift-xy-nm-per-sqrt-sec=20" })
   {
      const ScopeSpec rs = Spec(mode);
      LiveScope one, batch;
      for (int k = 0; k < 3; k++)
      {
         Take(one, rs);
         Take(batch, rs);
      }
      std::vector<uint16_t> seq, rec;
      const auto ts = std::chrono::steady_clock::now();
      for (int k = 0; k < 20; k++)
      {
         const Shot s = Take(one, rs);
         seq.insert(seq.end(), s.adu.begin(), s.adu.end());
      }
      const double seqMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
      LiveFrameInfo ri;
      std::string err;
      const auto tr = std::chrono::steady_clock::now();
      const bool rok = batch.Frames(rs, 20, true, rec, ri, err);
      const double recMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr).count();
      size_t same = 0;
      int maxDiff = 0;
      double sumSeq = 0, sumRec = 0;
      for (size_t i = 0; i < std::min(seq.size(), rec.size()); i++)
      {
         same += seq[i] == rec[i];
         maxDiff = std::max(maxDiff, std::abs(static_cast<int>(seq[i]) - static_cast<int>(rec[i])));
         sumSeq += seq[i];
         sumRec += rec[i];
      }
      std::printf("      max |diff| %d ADU, mean %.3f vs %.3f ADU\n", maxDiff, sumSeq / std::max<size_t>(1, seq.size()),
                  sumRec / std::max<size_t>(1, rec.size()));
      const Shot nextOne = Take(one, rs), nextBatch = Take(batch, rs);
      std::snprintf(msg, sizeof msg,
                    "recording %s: 20 frames at once = one by one, %.3f %% of pixels identical (%.0f ms, one by one "
                    "%.0f ms); the next live frame after it identical: %s",
                    *mode ? mode : "dSTORM", seq.size() ? 100.0 * same / seq.size() : 0.0, recMs, seqMs,
                    nextOne.adu == nextBatch.adu ? "yes" : "no");
      Check(rok && rec.size() == seq.size() && same >= 0.995 * seq.size() && maxDiff <= 4 && nextOne.adu == nextBatch.adu &&
            std::fabs(ri.tSec - 0.3) < 1e-9 && std::fabs(nextBatch.info.tSec - 2.3) < 1e-9, msg);
      if (!rok) std::printf("      (%s)\n", err.c_str());
   }

   std::printf("live_scope: %d failure(s), %.1f s\n", g_failures,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
