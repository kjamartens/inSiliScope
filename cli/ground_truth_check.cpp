// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
// ctest ground_truth: the ground truth a movie gives with a GroundTruthScope (Simulation/GroundTruth.h,
// docs/ground-truth.md).
//   - the frames are the same with and without it, bit for bit;
//   - a dSTORM nanoruler movie with beads: a row per frame; every row at least the threshold on the camera and no more
//     than its detected photons; a blink whose PSF window lies inside the image has all of them on the camera (the
//     kernel's planes sum to 1: within 1e-4, the sub-pixel interpolation's; no halo cut); every blink has its binding site; the beads are rows of kind fiducial;
//     the initial-ON dyes in the dye list but no dyes.csv among its files (localizations or a dye list, never both);
//     the same files from a second run;
//   - a WideField movie: dyes.csv and no localization table; every dye's photons at most its rate x the movie's time;
//     each density volume an ImageJ
//     float32 stack holding its dyes' emitted photons (the sum).
//
//   ground_truth_check          exit code 0 = all checks passed
#include "GroundTruth.h"
#include "ScopeMovie.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// A movie of spec (frames as one buffer), with gt in scope when given.
bool Movie(const std::string& text, GroundTruth* gt, std::vector<uint16_t>& frames, std::string& err)
{
   ScopeSpec spec;
   if (!ParseScopeSpec(text, spec, err))
      return false;
   frames.clear();
   GroundTruthScope scope(gt);
   ScopeMovieInfo info;
   return RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& adu) {
      frames.insert(frames.end(), adu.begin(), adu.end());
      return true;
   }, info, err);
}

std::string Join(const std::vector<std::pair<std::string, std::string>>& files)
{
   std::string s;
   for (const auto& f : files)
      s += f.first + '\n' + std::to_string(f.second.size()) + '\n' + f.second;
   return s;
}

// The float32 planes of a TIFF GroundTruth::Volumes wrote (its first IFD's strip offset; the planes back to back).
double TiffSum(const std::string& b, bool& imagej)
{
   uint32_t ifd;
   std::memcpy(&ifd, b.data() + 4, 4);
   uint16_t n;
   std::memcpy(&n, b.data() + ifd, 2);
   uint32_t w = 0, h = 0, off = 0, desc = 0, nz = 0;
   for (uint16_t i = 0; i < n; ++i)
   {
      const char* e = b.data() + ifd + 2 + 12 * i;
      uint16_t tag;
      uint32_t v;
      std::memcpy(&tag, e, 2);
      std::memcpy(&v, e + 8, 4);
      if (tag == 256) w = v;
      if (tag == 257) h = v;
      if (tag == 273) off = v;
      if (tag == 270) desc = v;
   }
   const std::string d = desc ? std::string(b.data() + desc) : std::string();
   imagej = d.compare(0, 12, "ImageJ=1.11a") == 0 && d.find("unit=micron") != std::string::npos;
   const size_t at = d.find("images=");
   nz = at == std::string::npos ? 1 : static_cast<uint32_t>(std::atol(d.c_str() + at + 7));
   double sum = 0;
   for (size_t i = 0; i < static_cast<size_t>(w) * h * nz; ++i)
   {
      float f;
      std::memcpy(&f, b.data() + off + 4 * i, 4);
      sum += f;
   }
   return sum;
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   std::string err;
   char b[400];
   const std::string base = "specimen=1 world-seed=7 seed=7 size=64 psf-kernel-half-width-nm=1500 psf-halo-cut=0 "
                            "blink-min-photons=0 disk-cache=0 ";
   // ---- dSTORM nanorulers with beads ----
   const std::string storm = base + "ruler-mode=dSTORM frames=40 start-sec=5 fid-density=0.5";
   std::vector<uint16_t> plain, with;
   GroundTruth gt;
   const bool ok = Movie(storm, nullptr, plain, err) && Movie(storm, &gt, with, err);
   if (!ok)
      std::printf("      (%s)\n", err.c_str());
   Check(ok && !plain.empty() && plain == with, "the frames with the ground truth's sink = without, bit for bit");
   Check(gt.frames.size() == 40 && gt.frames.back().frame == 39, "a row per frame (40)");
   const int r = 15 + 2;   // the PSF window's half width (1500 nm at 100 nm), and a pixel either side
   long rows = 0, inside = 0, beads = 0, sites = 0;
   double worst = 0, below = 0, over = 0;
   for (const GroundTruth::Emitter& e : gt.emitters)
   {
      if (e.kind == GroundTruth::Fiducial)
      {
         beads++;
         continue;
      }
      rows++;
      sites += std::isfinite(e.siteXUm);
      below = std::max(below, gt.minPhotons - e.onCamera);
      over = std::max(over, e.onCamera / e.detected - 1);
      if (e.xPx >= r && e.yPx >= r && e.xPx < 64 - r && e.yPx < 64 - r)
      {
         inside++;
         worst = std::max(worst, std::fabs(e.onCamera / e.detected - 1));
      }
   }
   std::snprintf(b, sizeof b, "%ld blink rows: each at least %g photons on the camera (worst short by %.3g), none more than "
                 "detected (worst over by %.2g)", rows, gt.minPhotons, below, over);
   Check(rows > 20 && below <= 0 && over < 1e-4, b);
   std::snprintf(b, sizeof b, "%ld blinks with their PSF window inside the image: all their photons on the camera (worst %.2g)",
                 inside, worst);
   Check(inside > 5 && worst < 1e-4, b);
   std::snprintf(b, sizeof b, "every blink row has its binding site (%ld of %ld)", sites, rows);
   Check(sites == rows, b);
   std::snprintf(b, sizeof b, "the beads are rows of kind fiducial (%ld)", beads);
   Check(beads > 0 && gt.Blinking(), b);
   std::snprintf(b, sizeof b, "the initial-ON dyes in the dye list (%zu)", gt.Dyes().size());
   Check(!gt.Dyes().empty(), b);
   const auto has = [](const std::vector<std::pair<std::string, std::string>>& files, const std::string& prefix) {
      for (const auto& f : files)
         if (f.first.compare(0, prefix.size(), prefix) == 0)
            return true;
      return false;
   };
   const auto stormFiles = gt.Files({ 5, 300 });
   Check(has(stormFiles, "localizations_min5") && !has(stormFiles, "dyes.csv"),
         "dSTORM: the localization tables, no dyes.csv");
   GroundTruth again;
   std::vector<uint16_t> f2;
   Check(Movie(storm, &again, f2, err) && Join(again.Files({ 5, 300 })) == Join(gt.Files({ 5, 300 })),
         "the same files from a second run");

   // ---- WideField ----
   const std::string wf = base + "ruler-mode=WideField frames=4 size=32";
   GroundTruth g2;
   std::vector<uint16_t> f3;
   const bool okW = Movie(wf, &g2, f3, err);
   double rateOver = 0, emitted = 0;
   for (const GroundTruth::Dye& d : g2.Dyes())
   {
      rateOver = std::max(rateOver, d.detectedTotal - d.photonsPerSec * 4 * 0.05 * (1 + 1e-12));
      emitted += d.emittedTotal;
   }
   std::snprintf(b, sizeof b, "WideField: %zu dyes, each at most its rate x the movie's 0.2 s (worst over by %.3g photons)",
                 g2.Dyes().size(), rateOver);
   Check(okW && g2.Dyes().size() > 10 && rateOver <= 0, b);
   const auto wfFiles = g2.Files({ 5, 300 }, 50, 25);
   Check(has(wfFiles, "dyes.csv") && !has(wfFiles, "localizations_"), "WideField: dyes.csv, no localization table");
   double volSum = 0;
   bool imagej = true;
   int vols = 0;
   for (const auto& f : wfFiles)
      if (f.first.compare(0, 8, "density_") == 0)
      {
         bool ij = false;
         volSum += TiffSum(f.second, ij);
         imagej = imagej && ij;
         vols++;
      }
   std::snprintf(b, sizeof b, "WideField: %d density volume(s), ImageJ float32 stacks holding the dyes' emitted photons "
                 "(%.6g vs %.6g)", vols, volSum, emitted);
   Check(vols == 1 && imagej && emitted > 0 && std::fabs(volSum / emitted - 1) < 1e-5, b);

   std::printf("ground_truth: %d failure(s), %.1f s\n", g_failures,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
