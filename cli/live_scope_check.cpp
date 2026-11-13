// ctest live_scope: live imaging of one sample (Simulation/LiveScope.h, the viewer's isc_live_*):
//   - the FOV centre's dose (LightHistory::DoseAt) grows by the same step per lit frame -- the exposure x the beam
//     there --, and not with the lasers off (or countLight 0);
//   - another place is fresh (clock 0), coming back finds the light it had;
//   - another world (world-seed) is a fresh sample: time and clocks back to 0;
//   - two sessions with the same requests give the same frames, bit for bit; the noise differs frame to frame;
//   - dSTORM's initial ON fades where the light has been and not elsewhere;
//   - a recording (Frames: n frames at once) = the same frames one by one (within one electron in a few pixels);
//   - a session replayed without rendering (Advance) gives the session's next frame, bit for bit;
//   - a moving WideField FOV gives the same frames with the moving shortcuts (SetScopeShortcuts) as without, and with
//     the spare time's prefetch (LiveScope::Prefetch) between frames, bit for bit.
//
//   live_scope_check     exit code 0 = all checks passed
#include "LiveScope.h"
#include "ScopeMovie.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
   // (wf-plane-nm 25, not the default 100 since 2026-11-11: a recording and live frames judge the light map's height
   // bands over different spans, and the bands' edges sit on the planes' boundaries; at 100 nm planes that moved the
   // WideField frames by up to 0.2 % against each other, inside the planes' own approximation but over these checks'
   // tolerances, which test the live session, not the plane thickness.)
   const std::string text = "size=32 exposure-ms=100 npc-dye=None mt-mode=dSTORM x=3 y=-2 z=0.5 illum-profile=FlatTop "
                            "psf-kernel-half-width-nm=2500 wf-plane-nm=25 " + extra;
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
   // The dose per lit frame at the FOV centre: the exposure x the beam there (the FlatTop's centre, within 0.1 %
   // of its peak).
   const double step = shots[1].info.litSec;
   bool ok = std::fabs(step - 0.1) < 1e-4;
   for (int k = 0; k < 6; k++)
      ok = ok && shots[k].ok && shots[k].info.lit && std::fabs(shots[k].info.litSec - step * k) < 1e-9 &&
           std::fabs(shots[k].info.tSec - 0.1 * k) < 1e-9;
   char msg[200];
   std::snprintf(msg, sizeof msg, "the FOV centre's dose and the time grow by one step per lit frame (%.6f s per 0.1 s "
                 "frame, 0 .. %.6f)", step, shots[5].info.litSec);
   Check(ok, msg);
   const Shot d1 = Take(live, dark), d2 = Take(live, dark);
   Check(d1.adu != d2.adu, "the noise differs frame to frame (two dark frames)");
   Check(d1.ok && d2.ok && !d1.info.lit && std::fabs(d1.info.litSec - 6 * step) < 1e-9 &&
         std::fabs(d2.info.litSec - 6 * step) < 1e-9, "lasers off: the frame lights nothing (the dose stays at 6 steps)");
   const Shot uncounted = Take(live, here, false), after = Take(live, here);
   Check(uncounted.ok && !uncounted.info.lit && std::fabs(after.info.litSec - 6 * step) < 1e-9,
         "countLight 0: the frame's light is not added");

   const Shot a = Take(live, away), back = Take(live, here);
   Check(a.ok && a.info.litSec == 0.0 && back.ok && std::fabs(back.info.litSec - 7 * step) < 1e-9,
         "40 um away is fresh (dose 0); back at the first place: 7 steps");

   // The initial ON (dSTORM): bright at a fresh place, fading under light.
   LiveScope fade;
   const ScopeSpec bright = Spec("laser-640=50");   // (8 frames: the dose of 80 at 5 mW)
   const Shot first = Take(fade, bright);
   Shot last;
   for (int k = 0; k < 8; k++)
      last = Take(fade, bright);
   const Shot fresh = Take(fade, Spec("x=-30 laser-640=50"));
   std::snprintf(msg, sizeof msg, "dSTORM initial ON fades under light: mean %.0f ADU at 0 s, %.0f after %.1f s; 33 um away fresh (dose %.1f s)",
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
   // A stepping pattern's WideField label (2026-11-11): live frames come from each state's kept bleach basis
   // (WidefieldScene::RenderFrameProgressive), a recording renders each state exactly, so they agree within the basis'
   // tolerance: the largest difference within 4 ADU + 1e-3 of the mean, the means within 1e-4.
   const int nRec = 8;
   for (const char* mode : { "", "mt-mode=DNA-PAINT", "drift-xy-nm-per-sqrt-sec=20",
                             "sim-type=SIM2D illum-bfp=0.5 mt-mode=WideField" })
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
      for (int k = 0; k < nRec; k++)
      {
         const Shot s = Take(one, rs);
         seq.insert(seq.end(), s.adu.begin(), s.adu.end());
      }
      const double seqMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
      LiveFrameInfo ri;
      std::string err;
      const auto tr = std::chrono::steady_clock::now();
      const bool rok = batch.Frames(rs, nRec, true, rec, ri, err);
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
                    "recording %s: %d frames at once = one by one, %.3f %% of pixels identical (%.0f ms, one by one "
                    "%.0f ms); the next live frame after it identical: %s",
                    *mode ? mode : "dSTORM", nRec, seq.size() ? 100.0 * same / seq.size() : 0.0, recMs, seqMs,
                    nextOne.adu == nextBatch.adu ? "yes" : "no");
      const bool pattern = std::string(mode).find("sim-type") != std::string::npos;
      const double meanSeq = sumSeq / std::max<size_t>(1, seq.size()), meanRec = sumRec / std::max<size_t>(1, rec.size());
      const bool close = pattern ? maxDiff <= 4 + 1e-3 * meanSeq && std::fabs(meanSeq - meanRec) <= 1e-4 * meanSeq
                                 : same >= 0.995 * seq.size() && maxDiff <= 4;
      Check(rok && rec.size() == seq.size() && close && nextOne.adu == nextBatch.adu &&
            std::fabs(ri.tSec - 0.3) < 1e-9 && std::fabs(nextBatch.info.tSec - (0.3 + 0.1 * nRec)) < 1e-9, msg);
      if (!rok) std::printf("      (%s)\n", err.c_str());
   }

   // A saved session replayed (Advance: the frames' light, drift, time and noise counter without rendering; the
   // viewer's Load settings, insiliscope_cli --settings): the next frame is the session's, bit for bit. The session's
   // frames as the viewer logs them: runs of the same spec as one call of n.
   for (const char* mode : { "", "mt-mode=DNA-PAINT", "drift-xy-nm-per-sqrt-sec=20",
                             "sim-type=SIM2D illum-bfp=0.5 mt-mode=WideField" })
   {
      const std::vector<std::pair<ScopeSpec, int>> runs = {
         { Spec(mode), 3 }, { Spec(std::string(mode) + " x=8"), 2 }, { Spec(std::string(mode) + " laser-561=0 laser-640=0 light-preset=-1"), 1 },
         { Spec(std::string(mode) + " laser-640=3"), 2 }, { Spec(mode), 2 } };
      LiveScope session, replay;
      bool aok = true;
      std::string err;
      for (const auto& r : runs)
         for (int k = 0; k < r.second; k++)
            Take(session, r.first);
      for (const auto& r : runs)
         aok = aok && replay.Advance(r.first, r.second, true, err);
      const Shot a = Take(session, Spec(mode)), b = Take(replay, Spec(mode));
      std::snprintf(msg, sizeof msg, "replay %s: the session's light without rendering, then the next frame = the session's "
                    "(t %.1f s, lit here %.4f s vs %.4f s)", *mode ? mode : "dSTORM", b.info.tSec, a.info.litSec, b.info.litSec);
      Check(aok && a.ok && b.ok && a.adu == b.adu && a.info.tSec == b.info.tSec && a.info.litSec == b.info.litSec &&
            a.info.drift.x == b.info.drift.x, msg);
      if (!aok) std::printf("      (%s)\n", err.c_str());
   }

   // The sample's frame counter (the frame program's input): every frame counts, dark and uncounted ones too; a
   // structured pattern's state follows it, and a fresh sample starts at 0.
   {
      LiveScope sim;
      const ScopeSpec pat = Spec("sim-type=SIM2D illum-bfp=0.5 mt-mode=WideField");
      std::vector<Shot> ps;
      for (int k = 0; k < 4; k++)
         ps.push_back(Take(sim, pat));
      bool cok = sim.FrameCounter() == 4;
      for (int k = 0; k < 4; k++)
         cok = cok && ps[static_cast<size_t>(k)].ok && ps[static_cast<size_t>(k)].info.frame == k &&
               ps[static_cast<size_t>(k)].info.patternPhase == k % 3 && ps[static_cast<size_t>(k)].info.patternAngle == k / 3 &&
               ps[static_cast<size_t>(k)].info.patternPeriod == 9;
      Take(sim, Spec("sim-type=SIM2D illum-bfp=0.5 mt-mode=WideField laser-561=0 laser-640=0 light-preset=-1"));
      Take(sim, pat, false);
      std::vector<uint16_t> adu;
      LiveFrameInfo fi;
      std::string err;
      cok = cok && sim.FrameCounter() == 6 && sim.Frames(pat, 3, true, adu, fi, err) && fi.frame == 6 &&
            fi.patternPhase == 0 && fi.patternAngle == 2 && sim.FrameCounter() == 9 && sim.Advance(pat, 2, true, err) &&
            sim.FrameCounter() == 11;
      const Shot c0 = Take(sim, pat);   // frame 11: angle 0 phase 2
      Check(cok && c0.info.frame == 11 && c0.info.patternAngle == 0 && c0.info.patternPhase == 2,
            "the frame counter: 4 frames, a dark one, an uncounted one, 3 at once, 2 advanced = 11; the pattern's state follows it");
      sim.Reset();
      Check(sim.FrameCounter() == 0, "Reset: the counter at 0");
      // A stack of 9 frames at once = 9 single frames in their pattern states (one movie's frames step the program).
      LiveScope one, many;
      std::vector<uint16_t> stack;
      LiveFrameInfo si;
      bool sok = many.Frames(pat, 9, true, stack, si, err);
      size_t same = 0;
      for (int k = 0; k < 9 && sok; k++)
      {
         const Shot sk = Take(one, pat);
         sok = sok && sk.ok;
         const size_t px = sk.adu.size();
         for (size_t i = 0; i < px; i++)
            same += std::abs(static_cast<int>(stack[static_cast<size_t>(k) * px + i]) - static_cast<int>(sk.adu[i])) <= 4;
      }
      Check(sok && same >= 0.995 * stack.size(),
            "9 frames at once step the pattern as 9 single frames do (" + std::to_string(same) + " of " +
               std::to_string(stack.size()) + " pixels within 4 ADU)");
   }

   // A moving FOV (2026-11-13): the stage steps 0.5 um in x and y (and the focus 0.1 um up) over WideField microtubules (mean-field) beside
   // sparsely labelled pores (per dye in every frame). The moving shortcuts (SetScopeShortcuts: a moved rect's counts
   // from the strips left and entered, the kernel spectra kept, only the pores' windows fetched) and the spare time's
   // prefetch (LiveScope::Prefetch) leave every frame the same, bit for bit.
   {
      auto at = [](int k) {
         ScopeSpec s;
         std::string err;
         char pos[64];
         std::snprintf(pos, sizeof pos, "x=%.2f y=%.2f z=%.2f ", 14.8 + 0.5 * (k / 2), -6.0 + 0.5 * (k % 3), 0.5 + 0.1 * k);
         ParseScopeSpec(std::string("size=48 exposure-ms=50 mt-dye=-1 mt-mode=WideField npc-dye=-1 "
                                    "npc-mode=WideField npc-label-pct=3 laser-488=0.01 dichroic=2 em-filter=2 "
                                    "psf-kernel-half-width-nm=2500 ") + pos, s, err);
         return s;
      };
      const int kSteps = 8;
      auto run = [&](bool shortcuts, bool prefetch, std::vector<std::vector<uint16_t>>& out, std::string& paths) {
         SetScopeShortcuts(shortcuts);
         LiveScope mv;
         bool ok = true;
         for (int k = 0; k < kSteps; k++)
         {
            const Shot s = Take(mv, at(k));
            ok = ok && s.ok;
            out.push_back(s.adu);
            paths = s.info.paths;
            if (prefetch)
               mv.Prefetch(at(k), 3.0, 30.0);
         }
         SetScopeShortcuts(true);
         return ok;
      };
      std::vector<std::vector<uint16_t>> on, off, pre;
      std::string pathsOn, pathsOff, pathsPre;
      const bool rok = run(true, false, on, pathsOn) && run(false, false, off, pathsOff) && run(true, true, pre, pathsPre);
      const bool both = pathsOn.find("mean-field") != std::string::npos && pathsOn.find("per dye") != std::string::npos;
      Check(rok && both && on == off,
            "a moving WideField FOV: the shortcuts (strip counts, kept kernels, the per-dye structures' windows only) "
            "give the frames of the whole-rect path, bit for bit (" + std::to_string(kSteps) + " poses; paths " + pathsOn + ")");
      Check(rok && on == pre, "the spare time's prefetch around the FOV changes no frame");
   }

   std::printf("live_scope: %d failure(s), %.1f s\n", g_failures,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
