// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
// ctest strips: a movie rendered in strips (ScopeMovie.h RenderScopeMovieStrips, docs/dev/LARGE_RECORDS.md part B)
// against the same movie rendered whole.
//   - blinks, beads, the slide, SIM, drift, dark frames: every pixel the same (the windows' events are the whole
//     field's, the camera maps and noise draws the sensor's);
//   - the mean field (WideField) and BrightField: within rounding (FFTs of the window's size; a Poisson draw may flip);
//   - the ground truth: the same rows (each blink, bead and dye once, in its strip), frames.csv the same;
//   - every frame's rows handed over once, strip by strip; uneven strips and a non-square sensor;
//   - the strip height: the whole sensor when it fits, less under a small budget; a side past 4096 px needs strips.
//
//   strips_check          exit code 0 = all checks passed
#include "GroundTruth.h"
#include "ScopeMovie.h"

#include <algorithm>
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
   if (!ok)
      g_failures++;
}

struct Run
{
   std::vector<uint16_t> frames;   // frame-major, the sensor's rows
   std::vector<std::pair<std::string, std::string>> files;
   unsigned w = 0, h = 0;
   long n = 0;
   bool rowsInOrder = true;
};

// A small margin (the PSF's and the mean field's reach: 2 um) so that 128 px has real strips.
const std::string kBase = "size=128 frames=4 psf-kernel-half-width-nm=2000 wf-kernel-um=2 ";

bool Render(const std::string& text, unsigned stripRows, Run& r, std::string& err)
{
   ScopeSpec spec;
   if (!ParseScopeSpec(text, spec, err))
      return false;
   GroundTruth gt;
   gt.minCorePhotons = 50;
   ScopeMovieInfo info;
   bool ok;
   {
      GroundTruthScope scope(&gt);
      if (stripRows == 0)
      {
         ok = RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& adu) {
            r.frames.insert(r.frames.end(), adu.begin(), adu.end());
            return true;
         }, info, err);
         r.w = info.width;
         r.h = info.height;
         r.n = info.frames;
      }
      else
      {
         unsigned W = 0, H = 0;
         long N = 0;
         ScopeMovieDims(spec, W, H, N);
         r.frames.assign(static_cast<size_t>(W) * H * N, 0);
         long nextF = 0;
         unsigned nextY = 0;
         ok = RenderScopeMovieStrips(spec, stripRows, [&](long f, unsigned y0, unsigned rows, const uint16_t* px) {
            // strip by strip, each strip's frames in order
            if (f != nextF || y0 != nextY)
               r.rowsInOrder = false;
            nextF = f + 1;
            if (nextF == N)
            {
               nextF = 0;
               nextY = y0 + rows;
            }
            std::copy(px, px + static_cast<size_t>(rows) * W, r.frames.begin() + (static_cast<size_t>(f) * H + y0) * W);
            return true;
         }, info, err);
         if (nextY != H)
            r.rowsInOrder = false;
         r.w = info.width;
         r.h = info.height;
         r.n = info.frames;
      }
   }
   r.files = gt.Files(50, 0, 0);
   return ok;
}

// A file's lines after the header, sorted (strips keep a frame's rows per strip).
std::vector<std::string> SortedRows(const std::string& csv)
{
   std::vector<std::string> rows;
   size_t at = csv.find('\n');
   while (at != std::string::npos && at + 1 < csv.size())
   {
      const size_t end = csv.find('\n', at + 1);
      rows.push_back(csv.substr(at + 1, (end == std::string::npos ? csv.size() : end) - at - 1));
      at = end;
   }
   std::sort(rows.begin(), rows.end());
   return rows;
}

// tolerance < 0: every pixel the same; else at most that share of pixels apart, and their mean |difference| at most
// `meanRel` of the mean signal above the offset.
void Compare(const char* name, const std::string& text, unsigned stripRows, double tolerance = -1, double meanRel = 0)
{
   const auto t0 = std::chrono::steady_clock::now();
   Run whole, strips;
   std::string err;
   const bool ok = Render(text, 0, whole, err) && Render(text, stripRows, strips, err);
   char b[512];
   std::snprintf(b, sizeof b, "%s: rendered whole and in strips of %u rows %s", name, stripRows, err.c_str());
   Check(ok, b);
   if (!ok)
      return;
   Check(whole.w == strips.w && whole.h == strips.h && whole.n == strips.n && whole.frames.size() == strips.frames.size(),
         std::string(name) + ": the same size");
   Check(strips.rowsInOrder, std::string(name) + ": every frame's rows once, strip by strip");
   if (whole.frames.size() != strips.frames.size())
      return;
   size_t apart = 0;
   double sumAbs = 0, sum = 0;
   for (size_t i = 0; i < whole.frames.size(); ++i)
   {
      const double d = std::fabs(static_cast<double>(whole.frames[i]) - strips.frames[i]);
      apart += d > 0;
      sumAbs += d;
      sum += whole.frames[i] > 100 ? whole.frames[i] - 100.0 : 0.0;
   }
   const double share = static_cast<double>(apart) / whole.frames.size(), rel = sum > 0 ? sumAbs / sum : 0;
   if (tolerance < 0)
      std::snprintf(b, sizeof b, "%s: every pixel the same (%zu of %zu apart)", name, apart, whole.frames.size());
   else
      std::snprintf(b, sizeof b, "%s: %.3g %% of the pixels apart (<= %.3g %%), mean |difference| %.2g of the signal (<= %.2g)",
                    name, 100 * share, 100 * tolerance, rel, meanRel);
   Check(tolerance < 0 ? apart == 0 : share <= tolerance && rel <= meanRel, b);
   // The ground truth: the same files, the tables' rows in another order within a frame.
   bool sameNames = whole.files.size() == strips.files.size();
   for (size_t k = 0; sameNames && k < whole.files.size(); ++k)
      sameNames = whole.files[k].first == strips.files[k].first;
   Check(sameNames, std::string(name) + ": the same ground-truth files");
   for (size_t k = 0; sameNames && k < whole.files.size(); ++k)
   {
      const std::string& f = whole.files[k].first;
      if (f.size() < 4 || f.compare(f.size() - 4, 4, ".csv") != 0)
         continue;
      const std::string &a = whole.files[k].second, &c = strips.files[k].second;
      const bool header = a.substr(0, a.find('\n')) == c.substr(0, c.find('\n'));
      const std::vector<std::string> ra = SortedRows(a), rc = SortedRows(c);
      std::snprintf(b, sizeof b, "%s: %s, %zu rows: the same rows%s", name, f.c_str(), ra.size(),
                    f == "frames.csv" ? " in the same order" : "");
      Check(header && ra == rc && (f != "frames.csv" || a == c), b);
   }
   std::printf("      (%s: %.2f s)\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   // Blinks (DNA-PAINT, the default scene) with beads; uneven strips.
   Compare("DNA-PAINT + beads", kBase + "fid-density=0.3", 40);
   // dSTORM (the dose map) in HILO with drift.
   Compare("dSTORM, HILO, drift", kBase + "mt-mode=dSTORM mt-dye=AF647 npc-dye=None illum-bfp=0.95 "
                                          "drift-xy-nm-per-sqrt-sec=20 drift-z-nm-per-sqrt-sec=5", 32);
   // PALM (activation).
   Compare("PALM", kBase + "mt-mode=PALM mt-dye=mEos3.2 npc-dye=None", 32);
   // SIM2D (a pattern per frame).
   Compare("SIM2D", kBase + "sim-type=1", 32);
   // The slide (its light per pixel from the sensor's beam).
   Compare("slide", kBase + "specimen=3", 32);
   // Dark frames (the camera maps and noise alone).
   Compare("dark", kBase + "light-epi=0 light-trans=0", 32);
   // A non-square sensor, strips not dividing it.
   Compare("non-square", "size=100 height=150 frames=3 psf-kernel-half-width-nm=2000 wf-kernel-um=2", 48);
   // The mean field (WideField): FFTs of each window's size.
   Compare("WideField", kBase + "mt-mode=WideField npc-dye=None", 32, 5e-3, 1e-4);
   // BrightField: the multislice over each window (its own margins).
   Compare("BrightField", kBase + "modality=1", 32, 0.6, 3e-4);
   {
      // The strip height.
      ScopeSpec spec;
      std::string err;
      ParseScopeSpec("size=1024", spec, err);
      Check(ScopeStripRows(spec, 32, 1e12) == 1024, "the whole sensor's rows when it fits the budget");
      const unsigned small = ScopeStripRows(spec, 32, 2e8);
      Check(small < 1024 && small >= 64, "fewer rows under a small budget (" + std::to_string(small) + ")");
      ParseScopeSpec("size=5000 height=200", spec, err);
      Check(ScopeNeedsStrips(spec, 32, 1e12), "a side past 4096 px needs strips");
      ParseScopeSpec("size=512", spec, err);
      Check(!ScopeNeedsStrips(spec, 32, 1e12), "512 px under a large budget: no strips");
   }
   std::printf("%s (%.1f s)\n", g_failures ? "FAILED" : "PASS",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
