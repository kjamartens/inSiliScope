// ctest fiducials: the fiducial beads' light and image (Simulation/Fiducials.h).
// Where the beads are is world_checks.fiducials.
//   - the light: the bleach half-time at the reference intensity = the option; a dark-red bead at 561 nm bleaches
//     ~100x slower (~1 % excitation); gold's laser scatter is blocked by the filters (OD 6) and bright without them;
//   - the bead kernel: it keeps the photons, a 200 / 400 nm bead is wider than a point, a 10 nm bead is a point;
//   - movies under a flat beam (every bead at one rate): the beads' photons decay frame by frame as one exponential,
//     through the static Taylor images (a 5 s half-time) and through per-frame splats (0.5 s), and a movie started one
//     half-time later has half the light.
//
//   fiducials_check          exit code 0 = all checks passed
#include "DyeLibrary.h"
#include "Fiducials.h"
#include "PsfGeneratorBridge.h"
#include "ScopeMovie.h"
#include "ScopeResolved.h"
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

int TypeIndex(const char* id)
{
   const auto& ids = FiducialTypeIds();
   return static_cast<int>(std::find(ids.begin(), ids.end(), id) - ids.begin());
}

void Light()
{
   char b[256];
   const Spectrum ones(kSpectraGridN, 1.0), zeros(kSpectraGridN, 0.0), half(kSpectraGridN, 0.5);
   auto lp = [&](double nm, const Spectrum& dichroic, const Spectrum& emission) {
      LightPath p;
      p.lasers = { { nm, 1.0 } };
      p.excitationT = ones;
      p.dichroicT = dichroic;
      p.emissionT = emission;
      p.qe = ones;
      p.eta = CollectionEfficiency(1.4, 1.518);
      return p;
   };
   FiducialLightOptions o;
   o.dyes = -1;
   o.halfSec = 28800;
   o.refKWcm2 = 1;
   o.blockingOD = 6;
   std::string err;
   std::vector<FiducialComponent> red, red561, blocked, open;
   const int t = TypeIndex("FluoSpheresDarkRed"), g = TypeIndex("Gold");
   const bool ok = FiducialPhotophysics(t, lp(651, zeros, ones), o, red, err) &&
                   FiducialPhotophysics(t, lp(561, zeros, ones), o, red561, err) &&
                   FiducialPhotophysics(g, lp(561, zeros, zeros), o, blocked, err) &&
                   FiducialPhotophysics(g, lp(561, half, ones), o, open, err);
   if (!ok || red.empty() || red561.empty())
   {
      std::printf("  %s\n", err.c_str());
      Check(false, "the bead light: FiducialPhotophysics");
      return;
   }
   std::snprintf(b, sizeof b, "bleach half-time at the reference = the option (%.1f s vs 28800)", std::log(2.0) / red[0].bleachPerSec);
   Check(std::fabs(std::log(2.0) / red[0].bleachPerSec / 28800 - 1) < 1e-9, b);
   const double r561 = red561[0].bleachPerSec / red[0].bleachPerSec;
   std::snprintf(b, sizeof b, "dark red at 561 nm: ~1 %% excitation, ~100x slower bleaching (x %.4f)", r561);
   Check(r561 > 0.003 && r561 < 0.03, b);
   // A dichroic that reflects the laser to the sample also reflects its scatter back: only the blocking leaks it. A
   // 50/50 beam splitter and no emission filter let it through.
   const auto scatter = [](const std::vector<FiducialComponent>& v) {
      for (const FiducialComponent& c : v)
         if (c.scatter) return c.perUm2;
      return -1.0;
   };
   const double sb = scatter(blocked), so = scatter(open), xs = GoldScatteringUm2(561, 100);
   std::snprintf(b, sizeof b, "gold scatter: blocked at OD 6, bright without filters (%.2e vs %.2e photons/s per 100 nm bead)",
                 sb * xs, so * xs);
   Check(sb > 0 && so / sb > 1e5, b);
}

void Kernel()
{
   char b[256];
   ScopeSpec spec;
   PsfKernelCache K;
   std::string err;
   if (!ParseScopeSpec("psf-kernel-half-width-nm=3000", spec, err) || !ScopePsfKernel(spec, 680, K, err) || !K.valid)
   {
      std::printf("  %s\n", err.c_str());
      Check(false, "the bead kernel: the point kernel at 680 nm");
      return;
   }
   BeadKernel B(K, 100);
   const unsigned N = 61;
   const double c = 30.2;
   const int k = K.NearestZIndex(0);
   auto img = [&](double d) {
      std::vector<float> a(N * N, 0.0f);
      if (d > 0)
         B.Splat(a, N, N, k, d, c, c, 1.0);
      else
         SplatPsfKernel(a, N, N, K, k, c, c, 1.0, K.interpMode);
      return a;
   };
   auto sum = [](const std::vector<float>& a) { double s = 0; for (float v : a) s += v; return s; };
   // The centre's second moment (the halo dominates the full one): the sphere adds 2 r^2 / 5 in the plane.
   auto core = [&](const std::vector<float>& a) {
      double s = 0, r2 = 0;
      for (unsigned y = 0; y < N; y++)
         for (unsigned x = 0; x < N; x++)
         {
            const double d2 = (x - c) * (x - c) + (y - c) * (y - c);
            if (d2 > 9) continue;
            s += a[y * N + x];
            r2 += a[y * N + x] * d2;
         }
      return std::sqrt(r2 / s);
   };
   const std::vector<float> p0 = img(0), b10 = img(10), b200 = img(200), b400 = img(400);
   const double sp = sum(p0), s2 = sum(b200), s4 = sum(b400);
   std::snprintf(b, sizeof b, "bead kernel keeps the photons (%.5f, %.5f vs %.5f)", s2, s4, sp);
   Check(std::fabs(s2 / sp - 1) < 2e-3 && std::fabs(s4 / sp - 1) < 5e-3, b);
   const double w0 = core(p0), w2 = core(b200), w4 = core(b400);
   std::snprintf(b, sizeof b, "a 200 / 400 nm bead is wider than a point (core RMS %.3f -> %.3f -> %.3f px)", w0, w2, w4);
   Check(w4 > w2 && w2 > w0, b);
   double dmax = 0, peak = 0;
   for (unsigned i = 0; i < N * N; i++)
   {
      dmax = std::max(dmax, (double)std::fabs(b10[i] - p0[i]));
      peak = std::max(peak, (double)p0[i]);
   }
   std::snprintf(b, sizeof b, "a 10 nm bead = a point (max diff %.2f %% of the peak)", 100 * dmax / peak);
   Check(dmax / peak < 0.02, b);
}

// The beads' photons per frame of a movie.
std::vector<double> FrameSums(const std::string& text, bool& ok)
{
   ScopeSpec spec;
   ScopeMovieInfo info;
   std::string err;
   std::vector<double> sums;
   ok = ParseScopeSpec(text, spec, err) && RenderScopePhotons(spec, [&](long, const std::vector<float>& ph) {
           double s = 0;
           for (float v : ph) s += v;
           sums.push_back(s);
           return true;
        }, info, err);
   if (!ok)
      std::printf("  %s: %s\n", text.c_str(), err.c_str());
   return sums;
}

// The bead's bleach rate in a movie's light (the half-time option at the reference, x the excitation at the line).
double BleachPerSec(const std::string& text, double halfSec)
{
   ScopeSpec spec;
   ScopeResolved r;
   std::string err;
   std::vector<FiducialComponent> c;
   FiducialLightOptions o;
   o.halfSec = halfSec;
   if (!ParseScopeSpec(text, spec, err) || !ScopeResolve(spec, r, err) ||
       !FiducialPhotophysics(TypeIndex("FluoSpheresDarkRed"), r.lp, o, c, err) || c.empty())
   {
      std::printf("  %s\n", err.c_str());
      return 0;
   }
   return c[0].bleachPerSec;
}

void Movies()
{
   char b[256];
   const std::string base = "size=64 psf-kernel-half-width-nm=3000 mt-dye=None npc-dye=None light-preset=-1 laser-640=1 "
                            "illum-profile=Flat background-per-sec=0 fid-density=0.2 fid-type=FluoSpheresDarkRed";
   // Frame by frame under a flat beam (every bead one rate): sum_f / sum_0 = e^(-L f dt), through the Taylor images
   // (a 5 s half-time) and through per-frame splats (0.5 s: no order reaches the tolerance).
   for (double halfSec : { 5.0, 0.5 })
   {
      bool ok = false;
      std::snprintf(b, sizeof b, " frames=20 start-sec=0 fid-bleach-half-sec=%g", halfSec);
      const std::vector<double> s = FrameSums(base + b, ok);
      const double L = BleachPerSec(base + b, halfSec);
      double worst = 0;
      for (size_t f = 0; f < s.size(); f++)
         worst = std::max(worst, std::fabs(s[f] / s[0] / std::exp(-L * 0.05 * f) - 1));
      std::snprintf(b, sizeof b, "bleaching frame by frame at its rate (half-time %.2f s at 640 nm): worst %.1e off, %.3g photons "
                    "in frame 0", std::log(2.0) / L, worst, s.empty() ? 0.0 : s[0]);
      Check(ok && s.size() == 20 && s[0] > 0 && L > 0 && worst < 1e-5, b);
   }
   // The default half-time: a movie started one half-time later has half the light.
   bool ok1 = false, ok2 = false;
   const double L = BleachPerSec(base, FiducialDefaults().bleachHalfSec);
   std::snprintf(b, sizeof b, " frames=1 start-sec=%.17g", std::log(2.0) / L);
   const std::vector<double> s0 = FrameSums(base + " frames=1 start-sec=0", ok1), sh = FrameSums(base + b, ok2);
   const double q = ok1 && ok2 ? sh.at(0) / s0.at(0) : 0;
   std::snprintf(b, sizeof b, "the bead signal halves after its half-time (%.4f after %.0f s at 640 nm)", q, std::log(2.0) / L);
   Check(L > 0 && std::fabs(q - 0.5) < 2e-3, b);
   // The beads' depth: not the dyes' cut (z-range-um, 7: +-3.5 um), which once dropped every bead 3.5 um off focus;
   // beyond their own stack (fid-psf-z-range-um) a Gaussian growing with the defocus, faded in (Fiducials.h
   // kBeadFarFadeUm): no step at the stack's edge; fid-far-defocus 0 draws none there.
   bool ok3 = false, ok4 = false, ok5 = false, ok6 = false, ok7 = false, ok8 = false, ok9 = false;
   const std::string glass = base + " frames=1 start-sec=0 fid-height=Coverslip";
   const std::string seven = glass + " fid-psf-z-range-um=7";
   const double sIn = FrameSums(glass + " z=0.1", ok3).at(0), sDyeCut = FrameSums(glass + " z=0.1 z-range-um=1", ok4).at(0);
   const double s5 = FrameSums(glass + " z=5.1", ok5).at(0);
   const double sInside = FrameSums(seven + " z=3.06", ok6).at(0), sOutside = FrameSums(seven + " z=3.66", ok7).at(0);
   const double sFar = FrameSums(seven + " z=8.1", ok8).at(0), sOff = FrameSums(seven + " z=5.1 fid-far-defocus=0", ok9).at(0);
   std::snprintf(b, sizeof b, "the beads' depth: 5 um off focus %.3g photons (%.3g in focus), the dyes' z-range-um 1 leaves "
                 "them (%.3g); a 7 um stack: %.4g / %.4g photons 0.5 um inside / beyond its edge, %.3g at 8 um, %.3g with "
                 "fid-far-defocus 0", s5, sIn, sDyeCut, sInside, sOutside, sFar, sOff);
   Check(ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && s5 > 0.01 * sIn && sDyeCut == sIn &&
         std::fabs(sOutside / sInside - 1) < 0.15 && sFar > 0.01 * sIn && sOff == 0, b);
}

} // namespace

int main()
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);   // progress shows as it runs (ctest, pipes)
   const auto t0 = std::chrono::steady_clock::now();
   Light();
   Kernel();
   Movies();
   std::printf("fiducials check: %.1f s, %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
               g_failures ? "FAILED" : "PASS");
   return g_failures ? 1 : 0;
}
