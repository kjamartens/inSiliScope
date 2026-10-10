// ctest blink_regimes: the three blink render regimes (spec/ALGORITHM.md "Blink render regimes").
//   * ExpectedBlinkOnSeconds (BlinkExpectation.h) = the mean ON time of the core's own blinks over ~1e5 dyes
//     (dSTORM, PALM, DNA-PAINT; one set of kinetics and an A/B/A rate history), within the Monte Carlo error;
//   * the binned renderer (BinnedBlinks.h) = the splat on one set of emitters: the same photons, a few % per pixel
//     (the position snapped to the grid), the same result serial and on all cores;
//   * movies: the binned and the mean-field movie's total signal = the splat's (DNA-PAINT, dense);
//   * the defaults leave a sparse movie to the splat (the same ADU as with both regimes off).
//
//   blink_regimes_check          exit code 0 = all checks passed
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "BinnedBlinks.h"
#include "BlinkExpectation.h"
#include "Parallel.h"
#include "PsfGeneratorBridge.h"
#include "SMLMSimulation.h"
#include "ScopeMovie.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      g_failures++;
}

struct Kin
{
   double act, on, off, bleach, initialOn;
};

// ---- the expected ON time against the core's blinks ----
void ExpectationCase(const char* name, int mode, const std::vector<Kin>& kin, const std::vector<double>& tStart,
                     double t0, double t1)
{
   const auto tq = std::chrono::steady_clock::now();
   IscParams* params = isc_params_new();
   IscWorld* w = isc_world_new(1249, params);
   isc_params_free(params);
   std::vector<double> label(ISC_LABEL_COUNT, 0.0);
   label[ISC_LABEL_DENSITY] = 0.7;
   label[ISC_LABEL_FLUORESCENT_FRACTION] = 1.0;
   label[ISC_LABEL_MODE] = mode;
   label[ISC_LABEL_ACTIVATION_RATE] = kin[0].act;
   label[ISC_LABEL_ON_SEC] = kin[0].on;
   label[ISC_LABEL_OFF_SEC] = kin[0].off;
   label[ISC_LABEL_BLEACH_PROB] = kin[0].bleach;
   label[ISC_LABEL_PHOTON_CV] = 0.0;
   label[ISC_LABEL_INITIAL_ON_SEC] = kin[0].initialOn;
   label[ISC_LABEL_ORIENT_POLAR_DEG] = 90;
   isc_world_set_label(w, ISC_STRUCT_MICROTUBULE, label.data(), ISC_LABEL_COUNT);
   std::vector<BlinkKineticsSegment> segs;
   std::vector<double> rows;
   for (size_t i = 0; i < kin.size(); ++i)
   {
      BlinkKineticsSegment s;
      s.tStart = tStart[i];
      s.activationRatePerSec = kin[i].act;
      s.onSec = kin[i].on;
      s.offSec = kin[i].off;
      s.bleachProb = kin[i].bleach;
      s.initialOnSec = kin[i].initialOn;
      segs.push_back(s);
      rows.push_back(tStart[i]);
      for (int st = 0; st < ISC_STRUCT_COUNT; ++st)
      {
         const double k[ISC_KIN_COUNT] = { kin[i].act, kin[i].on, kin[i].off, kin[i].bleach, 0.0, kin[i].initialOn };
         rows.insert(rows.end(), k, k + ISC_KIN_COUNT);
      }
   }
   if (kin.size() > 1)
      isc_world_set_kinetics_history(w, rows.data(), static_cast<int32_t>(kin.size()));
   const double x0 = 61, y0 = 2, x1 = 63.5, y1 = 4.5, zMin = -50, zMax = 50;
   float count = 0.0f;
   const int32_t nDyes = isc_density3d_in_window(w, x0, y0, x1, y1, zMin, zMax, 1, 1, 1, 1, &count);
   std::vector<double> ev(1 << 20);
   int32_t n = isc_events_in_window(w, x0, y0, x1, y1, zMin, zMax, t0, t1, ev.data(), static_cast<int32_t>(ev.size() / ISC_EVENT_STRIDE));
   if (n > static_cast<int32_t>(ev.size() / ISC_EVENT_STRIDE))
   {
      ev.resize(static_cast<size_t>(n) * ISC_EVENT_STRIDE);
      n = isc_events_in_window(w, x0, y0, x1, y1, zMin, zMax, t0, t1, ev.data(), n);
   }
   isc_world_free(w);
   // Sum of the ON overlaps; its variance ~ the sum over dyes of each dye's
   // squared ON time (the dyes are independent, one dye's blinks are not).
   double sum = 0.0, sum2 = 0.0;
   std::map<double, double> perDye;
   for (int32_t i = 0; i < n; ++i)
   {
      const double* e = &ev[static_cast<size_t>(i) * ISC_EVENT_STRIDE];
      if (static_cast<int>(e[8]) != ISC_STATE_BLINK)
         continue;
      const double ov = std::min(e[4], t1) - std::max(e[3], t0);
      if (ov > 0)
      {
         sum += ov;
         perDye[e[6]] += ov;
      }
   }
   for (const auto& d : perDye)
      sum2 += d.second * d.second;
   const double expect = nDyes * ExpectedBlinkOnSeconds(mode, segs, t0, t1);
   const double sigma = std::sqrt(sum2);
   const double z = sigma > 0 ? (sum - expect) / sigma : 0.0;
   char b[300];
   std::snprintf(b, sizeof b, "%s: %d dyes, [%g, %g) s: ON %.2f s, expected %.2f s (%+.2f %%, %.1f sigma, %d blinks)", name,
                 nDyes, t0, t1, sum, expect, 100 * (sum / std::max(1e-12, expect) - 1), z, n);
   std::printf("    (%.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - tq).count());
   Check(nDyes > 10000 && n > 300 && std::fabs(z) < 4.0, b);
}

void ExpectationVsCore()
{
   const Kin storm = { 0.002, 0.02, 3.0, 0.05, 8.0 };
   const Kin storm2 = { 0.02, 0.01, 1.0, 0.1, 8.0 };
   ExpectationCase("dSTORM, initial ON", ISC_MODE_DSTORM, { storm }, { 0 }, 2.0, 12.0);
   ExpectationCase("dSTORM, steady", ISC_MODE_DSTORM, { storm }, { 0 }, 60.0, 70.0);
   ExpectationCase("dSTORM, history A/B/A", ISC_MODE_DSTORM, { storm, storm2, storm }, { 0, 20.5, 40.25 }, 38.0, 48.0);
   const Kin palm = { 0.01, 0.05, 2.0, 0.3, 0.0 };
   const Kin palm2 = { 0.1, 0.05, 2.0, 0.3, 0.0 };
   ExpectationCase("PALM", ISC_MODE_PALM, { palm }, { 0 }, 30.0, 40.0);
   ExpectationCase("PALM, history A/B/A", ISC_MODE_PALM, { palm, palm2, palm }, { 0, 10.3, 25.0 }, 20.0, 30.0);
   const Kin paint = { 0.05, 0.3, 1.0, 1.0, 0.0 };
   const Kin paint2 = { 0.2, 0.15, 1.0, 1.0, 0.0 };
   ExpectationCase("DNA-PAINT", ISC_MODE_DNA_PAINT, { paint }, { 0 }, 60.0, 61.0);
   ExpectationCase("DNA-PAINT, from t = 0", ISC_MODE_DNA_PAINT, { paint }, { 0 }, 0.0, 1.5);
   ExpectationCase("DNA-PAINT, history A/B/A (bins straddled)", ISC_MODE_DNA_PAINT, { paint, paint2, paint },
                   { 0, 20.4, 30.0 }, 19.7, 21.3);
}

// ---- the binned renderer against the splat ----
void BinnedVsSplat()
{
   ScopeSpec spec;
   std::string err;
   PsfKernelCache kernel;
   if (!ParseScopeSpec("psf-kernel-half-width-nm=3000", spec, err) || !ScopePsfKernel(spec, 670.0, kernel, err))
   {
      Check(false, ("PSF kernel: " + err).c_str());
      return;
   }
   const PsfKernelCache blink = WithHaloCut(kernel, ScopeSpecGet(spec, "psf-halo-cut"));
   const unsigned W = 64, H = 64;
   const double pixelNm = 100, perBlink = 1000, zStage = 0.5;
   std::mt19937_64 rng(7);
   std::uniform_real_distribution<double> ux(-0.5, 6.9), uz(-500, 2500), ut(-0.3, 1.3);
   std::vector<BlinkEvent> events(3000);
   for (BlinkEvent& e : events)
   {
      e.xUm = ux(rng);
      e.yUm = ux(rng);
      e.zNm = uz(rng);
      e.tStart = ut(rng);
      e.tEnd = e.tStart + 0.6;
   }
   std::vector<float> splat(W * H, 0.0f);
   RenderPhotonImage(splat, W, H, events, 0, pixelNm, 1.0, perBlink, 0.0, 0.0, 0.0, &blink, zStage);
   for (int u : { 1, 2, 3 })
   {
      BinnedBlinkRenderer r;
      r.Setup(&blink, 1.0, W, H, u);
      std::vector<FrameEmitter> ems;
      CollectFrameEmitters(events, 0, W, H, pixelNm, perBlink, 0.0, 0.0, &blink, zStage, nullptr, ems);
      std::vector<float> a(W * H, 0.0f), b(W * H, 0.0f);
      r.Render(ems, a);   // FFT lines on all cores
      ++ParallelDepth();  // as inside a frame-parallel batch: serial
      r.Render(ems, b);
      --ParallelDepth();
      double sa = 0, ss = 0, d2 = 0, s2 = 0;
      for (size_t i = 0; i < a.size(); ++i)
      {
         sa += a[i];
         ss += splat[i];
         d2 += (a[i] - splat[i]) * static_cast<double>(a[i] - splat[i]);
         s2 += static_cast<double>(splat[i]) * splat[i];
      }
      const double rel = std::sqrt(d2 / s2);
      char t[240];
      std::snprintf(t, sizeof t, "binned (upscale %d, R %d cells) = splat: photons %+.3f %%, image rel L2 %.4f, serial = parallel",
                    r.Upscale(), r.RadiusCells(), 100 * (sa / ss - 1), rel);
      // The binned kernel keeps the cells the halo cut drops (+0.x %); the snap
      // to the grid moves each spot by up to half a cell.
      const double relMax = u == 1 ? 0.25 : u == 2 ? 0.08 : 0.06;
      Check(std::fabs(sa / ss - 1) < 0.01 && rel < relMax && a == b, t);
   }
}

// ---- movies ----
double MovieTotal(const std::string& text, double* fluct = nullptr)
{
   ScopeSpec spec;
   std::string err;
   ScopeMovieInfo info;
   double total = 0.0;
   std::vector<double> sum, sum2;
   long frames = 0;
   if (!ParseScopeSpec(text, spec, err) ||
       !RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& adu) {
          sum.resize(adu.size(), 0.0);
          sum2.resize(adu.size(), 0.0);
          for (size_t i = 0; i < adu.size(); ++i)
          {
             total += adu[i];
             sum[i] += adu[i];
             sum2[i] += static_cast<double>(adu[i]) * adu[i];
          }
          frames++;
          return true;
       }, info, err))
      std::printf("  movie failed: %s\n", err.c_str());
   if (fluct && frames > 1)
   {
      double s = 0;
      for (size_t i = 0; i < sum.size(); ++i)
         s += std::sqrt(std::max(0.0, sum2[i] / frames - (sum[i] / frames) * (sum[i] / frames)));
      *fluct = s / sum.size();
   }
   return total / std::max(1L, frames);
}

void Movies()
{
   // Photon-counting camera without offset, so the ADU sum is the signal (+ the imager background).
   const std::string base = "size=40 frames=40 world-seed=1249 x=63 y=3 mt-imager-nm=20 psf-kernel-half-width-nm=2500 "
                            "gain=1 offset=0 offset-std=0 read-noise=0 dark-per-sec=0 gain-std-pct=0 read-noise-std-pct=0 ";
   double fs = 0, fb = 0, fm = 0;
   const double splat = MovieTotal(base + "blink-binned-density-per-um2=1e9", &fs);
   const double binned = MovieTotal(base + "blink-binned-max-emitters=0", &fb);
   const double mf = MovieTotal(base + "blink-mean-field-max-emitters=0", &fm);
   char b[260];
   std::snprintf(b, sizeof b, "DNA-PAINT 20 nM movie: binned total %+.2f %%, temporal std %.1f vs %.1f", 100 * (binned / splat - 1), fb, fs);
   Check(std::fabs(binned / splat - 1) < 0.01 && std::fabs(fb / fs - 1) < 0.05, b);
   std::snprintf(b, sizeof b, "DNA-PAINT 20 nM movie: mean-field total %+.2f %%, temporal std %.1f (shot noise only) vs %.1f", 100 * (mf / splat - 1), fm, fs);
   Check(std::fabs(mf / splat - 1) < 0.02 && fm < 0.8 * fs, b);
}

void DefaultsSparse()
{
   auto adu = [](const std::string& text) {
      ScopeSpec spec;
      std::string err;
      ScopeMovieInfo info;
      std::vector<uint16_t> all;
      if (ParseScopeSpec(text, spec, err))
         RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& a) {
            all.insert(all.end(), a.begin(), a.end());
            return true;
         }, info, err);
      return all;
   };
   const std::string base = "size=32 frames=4 world-seed=1249 x=63 y=3 psf-kernel-half-width-nm=2500 ";
   const auto def = adu(base);
   const auto off = adu(base + "blink-binned-density-per-um2=1e9 blink-binned-max-emitters=1e9 "
                               "blink-mean-field-density-per-um2=1e9 blink-mean-field-max-emitters=1e9");
   Check(!def.empty() && def == off, "defaults: a sparse DNA-PAINT movie is splatted (the same ADU as both regimes off)");
}

} // namespace

int main()
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   const auto t0 = std::chrono::steady_clock::now();
   auto lap = [&](const char* what) {
      std::printf("  (%s done at %.1f s)\n", what, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   };
   ExpectationVsCore();
   lap("expectation");
   BinnedVsSplat();
   lap("binned");
   Movies();
   lap("movies");
   DefaultsSparse();
   std::printf("%s (%.1f s)\n", g_failures ? "FAILED" : "all passed",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
