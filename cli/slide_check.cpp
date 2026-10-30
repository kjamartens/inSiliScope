// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
// ctest slide: the fluorescent slide (specimen 3, Simulation/Slide.h).
//   - the light: the bleach half-time at 1 kW/cm^2 at the dye's excitation peak = the option (a library dye and the
//     Mix's TetraSpeck components); the photons per pixel = concentration x N_A x pixel area x the dye's detected rate;
//   - movies under a flat epi beam: the photons per pixel = that x thickness x exposure, linear in the thickness;
//     bleaching frame by frame as one exponential, a movie one half-time later at half the light;
//   - TIRF lights only the layer's bottom (a thicker layer adds nothing, far less light than epi); HILO's tilted
//     sheet and a Gaussian beam bleach its centre first;
//   - a host's light history: a movie after t seconds of the same light = a movie at start-sec t (flat and Gaussian).
//
//   slide_check          exit code 0 = all checks passed
#include "DyeLibrary.h"
#include "LightHistory.h"
#include "ScopeMovie.h"
#include "ScopeResolved.h"
#include "Slide.h"
#include "Spectra.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

void Light()
{
   char b[256];
   const Spectrum ones(kSpectraGridN, 1.0);
   // Every line reaches the sample (a dichroic reflecting below the longest line + 5 nm), the emission above it passes.
   auto lp = [&](const std::vector<LaserLine>& lines) {
      LightPath p;
      p.lasers = lines;
      p.excitationT = ones;
      double top = 0;
      for (const LaserLine& l : lines)
         top = std::max(top, l.nm);
      p.dichroicT = Spectrum(kSpectraGridN, 0.0);
      for (int i = 0; i < kSpectraGridN; ++i)
         p.dichroicT[static_cast<size_t>(i)] = SpectraGridNm(i) > top + 5 ? 1.0 : 0.0;
      p.emissionT = ones;
      p.qe = ones;
      p.eta = CollectionEfficiency(1.4, 1.518);
      return p;
   };
   std::string err;
   // A library dye at its excitation peak at 1 kW/cm^2: the half-time is the option's.
   const int cy3b = IndexOf(SlideDyeNames(), "Cy3B");
   const DyeData& d = DyeAt(cy3b - 1);
   SlideOptions o;
   o.dye = cy3b;
   o.halfSec = 600;
   o.concMicroM = 2;
   std::vector<SlideComponent> c;
   const bool ok = SlidePhotophysics(o, lp({ { d.main.exMaxNm, 1.0 } }), 0.1, c, err);
   std::snprintf(b, sizeof b, "Cy3B at its peak (%g nm), 1 kW/cm^2: half-time %.6g s (option 600)", d.main.exMaxNm,
                 ok && c.size() == 1 ? std::log(2.0) / c[0].bleachPerSec : 0.0);
   Check(ok && c.size() == 1 && std::fabs(std::log(2.0) / c[0].bleachPerSec / 600 - 1) < 1e-9, b);
   // Its photons per pixel per um: c N_A x pixel area x the state's detected rate.
   StatePhysics st;
   const bool okSt = StatePhotophysics(d.main, lp({ { d.main.exMaxNm, 1.0 } }), st, err);
   const double want = 2e-6 * 6.02214076e23 * 1e-15 * 0.01 * st.detectedPerSec;
   std::snprintf(b, sizeof b, "photons per pixel per um = c N_A x pixel^2 x detected rate (%.6g vs %.6g)",
                 okSt && !c.empty() ? c[0].perPxUmSec : 0.0, want);
   Check(okSt && !c.empty() && std::fabs(c[0].perPxUmSec / want - 1) < 1e-12, b);
   // The Mix: TetraSpeck's components, each at its peak bleaching at the option's half-time; four lines light all four.
   const FiducialTypeData& T = FiducialTypeAt(IndexOf(FiducialTypeIds(), "TetraSpeck"));
   o.dye = 0;
   bool all = true;
   for (int ci = 0; ci < T.componentCount; ++ci)
   {
      const FiducialComponentData& fc = FiducialComponentAt(T.firstComponent + ci);
      std::vector<SlideComponent> m;
      all = all && SlidePhotophysics(o, lp({ { fc.exPeakNm, 1.0 } }), 0.1, m, err);
      for (const SlideComponent& s : m)
         if (s.id == fc.id)
            all = all && std::fabs(std::log(2.0) / s.bleachPerSec / 600 - 1) < 1e-9;
   }
   // 405/488/561/640 with an open dichroic and emission path (half each way) excite and detect all four.
   std::vector<SlideComponent> four;
   LightPath open = lp({ { 405, 1 }, { 488, 1 }, { 561, 1 }, { 640, 1 } });
   open.dichroicT = Spectrum(kSpectraGridN, 0.5);
   all = all && SlidePhotophysics(o, open, 0.1, four, err);
   std::snprintf(b, sizeof b, "Mix: TetraSpeck's %d components, each at the half-time at its peak; 405/488/561/640 light %zu",
                 T.componentCount, four.size());
   Check(all && static_cast<int>(four.size()) == T.componentCount, b);
   // The light presets: the Mix's 561 nm, a dye's by its WideField preset's filters.
   std::snprintf(b, sizeof b, "light presets: Mix %s, AF647 %s, mEGFP %s, DAPI %s", SlideLightPresetId(0).c_str(),
                 SlideLightPresetId(IndexOf(SlideDyeNames(), "AF647")).c_str(),
                 SlideLightPresetId(IndexOf(SlideDyeNames(), "mEGFP")).c_str(),
                 SlideLightPresetId(IndexOf(SlideDyeNames(), "DAPI")).c_str());
   Check(SlideLightPresetId(0) == "Slide-561" && SlideLightPresetId(IndexOf(SlideDyeNames(), "AF647")) == "Slide-640" &&
            SlideLightPresetId(IndexOf(SlideDyeNames(), "mEGFP")) == "Slide-488" &&
            SlideLightPresetId(IndexOf(SlideDyeNames(), "DAPI")) == "Slide-405",
         b);
}

// The photon images of a spec's movie (no noise); with a history and its light, on them.
std::vector<std::vector<float>> Photons(const std::string& text, bool& ok, LightHistory* h = nullptr, double tSec = 0)
{
   ScopeSpec spec;
   std::string err;
   std::vector<std::vector<float>> out;
   ok = ParseScopeSpec(text, spec, err);
   LightState L;
   if (ok && h)
   {
      ok = MakeLightState(spec, true, L, err);
      if (ok && tSec > 0)
         h->Advance(tSec, L);
      spec["start-sec"] = h->T();
   }
   FluorescenceMovie fm;
   FluorescenceFrameOptions opt;
   opt.onPhotons = [&](long, const std::vector<float>& ph) {
      out.push_back(ph);
      return true;
   };
   ScopeMovieInfo info;
   ok = ok && fm.Begin(spec, false, err, nullptr, h, h ? &L : nullptr) &&
        fm.Render([](long, const std::vector<uint16_t>&) { return true; }, info, err, nullptr, &opt);
   if (!ok)
      std::printf("  %s: %s\n", text.c_str(), err.c_str());
   return out;
}

double Sum(const std::vector<float>& v)
{
   double s = 0;
   for (float x : v)
      s += x;
   return s;
}

// The slide's components in a spec's light path.
std::vector<SlideComponent> Components(const std::string& text)
{
   ScopeSpec spec;
   ScopeResolved r;
   std::string err;
   std::vector<SlideComponent> c;
   SlideOptions o;
   if (!ParseScopeSpec(text, spec, err) || !ScopeResolve(spec, r, err))
   {
      std::printf("  %s\n", err.c_str());
      return c;
   }
   o.dye = static_cast<int>(ScopeSpecGet(spec, "slide-dye"));
   o.concMicroM = ScopeSpecGet(spec, "slide-conc-micromolar");
   o.halfSec = ScopeSpecGet(spec, "slide-bleach-half-sec");
   if (!SlidePhotophysics(o, r.lp, ScopeSpecGet(spec, "pixel-nm") / 1000.0, c, err))
      std::printf("  %s\n", err.c_str());
   return c;
}

void Movies()
{
   char b[320];
   const std::string flat = "specimen=Slide size=48 illum-profile=Flat background-per-sec=0 frames=1 start-sec=0 slide-dye=Cy3B";
   bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
   // The photons: the layer's column under a flat epi beam, linear in the thickness.
   const std::vector<SlideComponent> c = Components(flat);
   const double n = 48.0 * 48, s5 = Sum(Photons(flat, ok1).at(0)), s25 = Sum(Photons(flat + " slide-thickness-um=2.5", ok2).at(0));
   const double want = c.empty() ? 0 : c[0].perPxUmSec * 5 * 0.05;
   std::snprintf(b, sizeof b, "flat epi: %.6g photons per pixel per frame (c N_A pixel^2 rate x 5 um x 50 ms: %.6g); 2.5 um: x %.6f",
                 s5 / n, want, s25 / s5);
   Check(ok1 && ok2 && c.size() == 1 && std::fabs(s5 / n / want - 1) < 1e-6 && std::fabs(s25 / s5 - 0.5) < 1e-6, b);
   // Bleaching frame by frame (a half-time of ~0.5 s here), and one half-time later half the light.
   const std::string fast = flat + " slide-bleach-half-sec=0.02";
   const std::vector<SlideComponent> cf = Components(fast);
   const double L = cf.empty() ? 0 : cf[0].bleachPerSec;
   const std::vector<std::vector<float>> fr = Photons(fast + " frames=20", ok3);
   double worst = 0;
   for (size_t f = 0; f < fr.size(); ++f)
      worst = std::max(worst, std::fabs(Sum(fr[f]) / Sum(fr[0]) / std::exp(-L * 0.05 * f) - 1));
   std::snprintf(b, sizeof b, " start-sec=%.17g", std::log(2.0) / L);
   const double half = Sum(Photons(fast + b, ok4).at(0)) / Sum(fr.at(0));
   std::snprintf(b, sizeof b, "bleaching at its rate (half-time %.3g s): frames worst %.1e off; one half-time later x %.6f",
                 std::log(2.0) / L, worst, half);
   Check(ok3 && ok4 && L > 0 && fr.size() == 20 && worst < 1e-5 && std::fabs(half - 0.5) < 1e-5, b);
   // TIRF: only the bottom ~0.1-0.2 um is lit: a thicker layer adds nothing, far less than epi.
   bool ok5 = false, ok6 = false;
   const double t5 = Sum(Photons(flat + " illum-bfp=1.03", ok5).at(0)), t25 = Sum(Photons(flat + " illum-bfp=1.03 slide-thickness-um=2.5", ok6).at(0));
   std::snprintf(b, sizeof b, "TIRF: %.4g photons per pixel (epi %.4g), 2.5 um layer x %.6f", t5 / n, s5 / n, t25 / t5);
   Check(ok5 && ok6 && t5 > 0 && t5 < 0.2 * s5 && std::fabs(t25 / t5 - 1) < 1e-4, b);
   // A Gaussian beam and HILO: the centre bleaches first (its share of the light drops with time).
   for (const char* g : { "", " illum-bfp=0.9" })
   {
      bool a = false, z = false;
      const std::string gs = std::string("specimen=Slide size=48 background-per-sec=0 frames=1 slide-dye=Cy3B slide-bleach-half-sec=0.02") + g;
      const std::vector<float> p0 = Photons(gs + " start-sec=0", a).at(0), p1 = Photons(gs + " start-sec=1", z).at(0);
      const size_t mid = 24 * 48 + 24, edge = 24 * 48 + 2;
      const double r0 = p0[mid] / p0[edge], r1 = p1[mid] / p1[edge];
      std::snprintf(b, sizeof b, "Gaussian beam%s: centre / edge %.4f fresh, %.4f after 1 s (the centre bleaches first)",
                    *g ? ", HILO" : ", epi", r0, r1);
      Check(a && z && r0 > 1 && r1 < 0.9 * r0, b);
   }
   // A host's light history: t seconds of the same light = start-sec t (each pixel's past light as seconds of the light
   // now), under a flat and a Gaussian beam and in HILO (each height at its own excitation).
   for (const char* g : { " illum-profile=Flat", "", " illum-bfp=0.9" })
   {
      bool a = false, z = false;
      const std::string gs = std::string("specimen=Slide size=48 background-per-sec=0 frames=3 slide-dye=Cy3B slide-bleach-half-sec=0.02") + g;
      LightHistory h;
      const std::vector<std::vector<float>> withH = Photons(gs, a, &h, 0.7);
      const std::vector<std::vector<float>> plain = Photons(gs + " start-sec=0.7", z);
      double w = 0;
      for (size_t f = 0; a && z && f < 3; ++f)
         for (size_t i = 0; i < plain[f].size(); ++i)
            w = std::max(w, std::fabs(static_cast<double>(withH[f][i]) / plain[f][i] - 1));
      std::snprintf(b, sizeof b, "light history (%s): 0.7 s of light = start-sec 0.7 (worst %.2e)",
                    !*g ? "Gaussian" : g[7] == 'p' ? "flat" : "HILO", w);
      Check(a && z && w < 1e-4, b);
   }
   // The past under another light (the user's report, 2026-10-30: HILO focused high after epi showed a step): 1 s of
   // epi, then HILO focused at 2.9 um, whose sheet meets the coverslip ~9 um aside, its 1e-4 footprint cut across the
   // field. Each pixel keeps the epi's bleaching (the column's past alike at every height): the HILO movie over a fresh
   // one = the epi movie after 1 s over a fresh one, pixel by pixel, within the history lattice's bilinear error
   // (~0.2 %; dose / the local rate made 1 on one side of the cut and ~0 on the other).
   {
      bool a = false, z = false, e = false, f = false;
      const std::string base = "specimen=Slide size=48 background-per-sec=0 frames=1 slide-dye=Cy3B slide-bleach-half-sec=0.02";
      const std::string epi = base + " illum-bfp=0 z=2", hilo = base + " illum-bfp=0.95 z=2.9";
      ScopeSpec sEpi;
      std::string err;
      LightState L;
      LightHistory h1, h2;
      const bool lit = ParseScopeSpec(epi, sEpi, err) && MakeLightState(sEpi, true, L, err);
      if (lit)
         h1.Advance(1.0, L);
      const std::vector<std::vector<float>> hWith = Photons(hilo, a, &h1);
      const std::vector<std::vector<float>> hFresh = Photons(hilo + " start-sec=0", z);
      const std::vector<std::vector<float>> eWith = Photons(epi, e, &h2, 1.0);
      const std::vector<std::vector<float>> eFresh = Photons(epi + " start-sec=0", f);
      double w = 0, lo = 1, hi = 0;
      bool all = lit && a && z && e && f && !hWith.empty() && !hFresh.empty() && !eWith.empty() && !eFresh.empty();
      for (size_t i = 0; all && i < hFresh[0].size(); ++i)
      {
         const double rh = static_cast<double>(hWith[0][i]) / hFresh[0][i], re = static_cast<double>(eWith[0][i]) / eFresh[0][i];
         all = hFresh[0][i] > 0 && eFresh[0][i] > 0;
         w = std::max(w, std::fabs(rh / re - 1));
         lo = std::min(lo, rh);
         hi = std::max(hi, rh);
      }
      std::snprintf(b, sizeof b, "epi's past under HILO focused high: its bleaching kept pixel by pixel (%.3f..%.3f of fresh, "
                    "worst %.1e off the epi's)", lo, hi, w);
      Check(all && hi < 0.9 && w < 5e-3, b);
   }
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   Light();
   Movies();
   std::printf("slide_check: %s (%.1f s)\n", g_failures ? "FAILED" : "all passed",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   return g_failures ? 1 : 0;
}
