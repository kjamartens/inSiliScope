// ctest excitation: the excitation geometry (Simulation/Excitation.h; JS twin web/prototype/scope/excitation.js, its
// checks web/lab/excitation_check.mjs).
//   - epi = the in-plane profile; FlatTop's half maximum at FWHM / 2; Flat epi is uniform;
//   - the field factor continuous at the critical angle (both sides approach (4 + 4 / n^2) / 2 / t(0)^2);
//   - the decay depth against Axelrod 1981's example (120 nm; axelrod1981);
//   - plane means and column integrals against quadrature (TIRF, two lines; HILO), the HILO sheet, a clipped spot dark;
//   - the free imager's collected column: isotropic against an independent focal-plane quadrature, the renderers' map
//     against a dense reference per regime, epi well below the chamber's height, a Flat beam's map = its columns;
//   - movies: a Flat HILO beam (one factor F everywhere) = epi with every laser line x F, per label mode (the
//     kinetics follow the local light); a clipped spot = a sample without light; TIRF cuts the nanorulers'
//     free-imager background.
//
//   excitation_check          exit code 0 = all checks passed
#include "Excitation.h"
#include "ScopeMovie.h"

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

bool Near(double a, double b, double tol)
{
   return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b));
}

ExcitationSpec Base(double bfp)
{
   ExcitationSpec s;
   s.profile = ILLUM_FLATTOP;
   s.fwhmPct = 100;
   s.fovWidthUm = 12.8;
   s.na = 1.49;
   s.nImm = 1.518;
   s.nMed = 1.337;
   s.pivotUm = 0;
   s.bfp = bfp;
   return s;
}

const ExcitationShares kRed = { { 640, 1.0 } };

void Units()
{
   char b[256];
   {
      const Excitation e(Base(0));
      Check(e.Regime() == REGIME_EPI && e.F() == 1.0, "bfp 0: epi, F = 1");
      Check(Near(e.At(3, 1, 2.5, kRed), e.ProfileAt(3, 1), 1e-15), "epi = the profile");
      Check(Near(e.ProfileAt(6.4, 0), 0.5, 2e-3), "FlatTop half maximum at FWHM / 2 (peak-normalised)");
      ExcitationSpec f = Base(0);
      f.profile = ILLUM_FLAT;
      Check(Excitation(f).Uniform() && !e.Uniform(), "Flat epi is uniform, FlatTop is not");
   }
   {
      const Excitation lo(Base(0.999)), hi(Base(1.001)), far(Base(0.99)), farHi(Base(1.01));
      const double n = 1.337 / 1.518, t0 = 2 * 1.518 / (1.518 + 1.337), lim = (4 + 4 / (n * n)) / 2 / (t0 * t0);
      std::snprintf(b, sizeof b, "F near the critical angle: %.4f %.4f | limit %.4f | %.4f %.4f", far.F(), lo.F(), lim,
                    hi.F(), farHi.F());
      Check(lo.Regime() == REGIME_HILO && hi.Regime() == REGIME_TIRF && far.F() < lo.F() && lo.F() < lim &&
               farHi.F() < hi.F() && std::fabs(hi.F() - lim) < 0.02 * lim,
            b);
   }
   {
      // Axelrod 1981 (Methods, Fig. 2): theta 72.5 deg in n1 = 1.44, theta_c 67.5 deg, 514.5 nm: d = 120 nm.
      const double pi = 3.14159265358979323846, n1 = 1.44, n2 = n1 * std::sin(67.5 * pi / 180);
      ExcitationSpec s = Base(n1 * std::sin(72.5 * pi / 180) / n2);
      s.nImm = n1;
      s.nMed = n2;
      const double d = Excitation(s).DepthNm(514.5);
      std::snprintf(b, sizeof b, "Axelrod 1981 decay depth: %.2f nm (120)", d);
      Check(Near(d, 120, 0.01), b);
   }
   {
      const ExcitationShares two = { { 640, 0.7 }, { 405, 0.3 } };
      const Excitation e(Base(1.05));
      const int n = 20000;
      double q = 0, c = 0;
      for (int i = 0; i < n; i++)
         q += e.Depth(0.05 + (i + 0.5) * 0.025 / n, two) / n;
      for (int i = 0; i < n; i++)
         c += e.At(1, 2, (i + 0.5) * 5.0 / n, two) * 5.0 / n;
      Check(Near(e.DepthMean(0.05, 0.075, two), q, 1e-9), "TIRF plane mean = quadrature");
      Check(Near(e.Column(1, 2, 5, two), c, 1e-6), "TIRF column integral = quadrature");
      ExcitationSpec hs = Base(0.9);
      hs.pivotUm = 2;
      const Excitation h(hs);
      double ch = 0;
      for (int i = 0; i < 200000; i++)
         ch += h.At(1, 2, (i + 0.5) * 100.0 / 200000, kRed) * 100.0 / 200000;
      Check(Near(h.Column(1, 2, 100, kRed), ch, 1e-3), "HILO column integral = quadrature");
      const double t = h.DepthNm() / 1000;
      std::snprintf(b, sizeof b, "HILO sheet %.2f um: lit at the pivot, dark a sheet above", t);
      Check(h.At(0, 0, 2, kRed) > 0.9 * h.F() && h.At(0, 0, 2 + t, kRed) < 0.01 * h.F(), b);
      Check(h.Column(0, 0, 100, kRed) < 0.2 * Excitation(Base(0)).Column(0, 0, 100, kRed), "HILO column well below epi");
   }
   {
      ExcitationSpec s = Base(1.1);
      s.na = 1.4;
      const Excitation e(s);
      Check(e.Regime() == REGIME_CLIPPED && e.At(0, 0, 0, kRed) == 0 && e.Column(0, 0, 10, kRed) == 0,
            "a spot beyond the pupil edge is dark");
   }
   {
      // The collected column (JS excitation_check's): 25.6 um FlatTop, NA 1.40, focus 0.5 um, a 100 um chamber.
      auto cb = [](double bfp) {
         ExcitationSpec s = Base(bfp);
         s.fovWidthUm = 25.6;
         s.na = 1.4;
         s.pivotUm = 0.5;
         return s;
      };
      const std::vector<ImagerSource> im = { { 1.0, kRed } };
      const double Hc = 100;
      const Excitation e(cb(0));
      // Isotropic, epi, on the axis: int dz int_0^inf P(dz t) t / (1 + t^2)^(3/2) dt (t = rho / dz, t = sinh s).
      double ref = 0;
      const int nz = 4000, ns = 3000;
      for (int i = 0; i < nz; i++)
      {
         const double dz = std::fabs((i + 0.5) * Hc / nz - 0.5), sMax = std::asinh(200 / std::max(dz, 1e-3));
         double inner = 0;
         for (int k = 0; k < ns; k++)
         {
            const double sk = (k + 0.5) * sMax / ns, t = std::sinh(sk);
            inner += e.ProfileAt(dz * t, 0) * t / std::pow(1 + t * t, 1.5) * std::cosh(sk) * sMax / ns;
         }
         ref += inner * Hc / nz;
      }
      const double iso = e.ImagerColumnAt(0, 0, Hc, im, 0, kCollectDirs, kCollectRatio, kCollectTirfSteps, false);
      const double fr = e.ImagerColumnAt(0, 0, Hc, im);
      std::snprintf(b, sizeof b, "isotropic collected column = the focal-plane quadrature (%.3f vs %.3f um)", iso, ref);
      Check(Near(iso, ref, 5e-3), b);
      std::snprintf(b, sizeof b, "epi collected column on the axis: Fresnel-weighted %.2f um, between isotropic %.2f and Hc / 5",
                    fr, iso);
      Check(fr > iso && fr < 0.2 * Hc, b);
      const unsigned W = 64;
      const double um = 0.4, ax = 12.8;
      for (const auto& c : { std::make_pair(0.0, 5e-3), std::make_pair(0.95, 3e-2), std::make_pair(1.03, 5e-3) })
      {
         const Excitation x(cb(c.first));
         const std::vector<double> m = x.ImagerMap(W, W, um, ax, ax, Hc, im);
         bool ok = true;
         double worst = 0;
         for (const auto& px : { std::make_pair(32u, 32u), std::make_pair(0u, 32u), std::make_pair(10u, 50u) })
         {
            const double d = x.ImagerColumnAt(px.first * um - ax, px.second * um - ax, Hc, im, 128, 64, 1.02, 512);
            const double v = m[static_cast<size_t>(px.second) * W + px.first];
            worst = std::max(worst, std::fabs(v / d - 1));
            ok = ok && Near(v, d, c.second);
         }
         std::snprintf(b, sizeof b, "%s collected-column map = the dense reference (worst %.2f %%)",
                       IlluminationRegimeNames()[static_cast<size_t>(x.Regime())].c_str(), 100 * worst);
         Check(ok, b);
      }
      ExcitationSpec fs = cb(0.9);
      fs.profile = ILLUM_FLAT;
      const Excitation f(fs);
      const std::vector<double> fm = f.ImagerMap(8, 8, 1, 4, 4, Hc, im);
      Check(fm[3 * 8 + 5] == f.Column(1, -1, Hc, kRed), "a Flat beam's collected-column map = its columns");
   }
}

std::vector<uint16_t> Movie(const std::string& text, bool& ok)
{
   ScopeSpec spec;
   ScopeMovieInfo info;
   std::string err;
   std::vector<uint16_t> all;
   ok = ParseScopeSpec(text, spec, err) &&
        RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& a) { all.insert(all.end(), a.begin(), a.end()); return true; },
                         info, err);
   if (!ok)
      std::printf("  %s: %s\n", text.c_str(), err.c_str());
   return all;
}

double Identical(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
{
   if (a.size() != b.size() || a.empty())
      return 0;
   size_t same = 0;
   for (size_t i = 0; i < a.size(); ++i)
      same += a[i] == b[i];
   return static_cast<double>(same) / a.size();
}

double Mean(const std::vector<uint16_t>& a)
{
   double s = 0;
   for (uint16_t v : a)
      s += v;
   return a.empty() ? 0 : s / a.size();
}

void Movies()
{
   char b[512];
   ExcitationSpec es = Base(0.6);
   es.profile = ILLUM_FLAT;
   es.na = 1.4;
   const double F = Excitation(es).F();
   const std::string B = "world-seed=1249 x=63 y=3 size=32 psf-model=0 npc-dye=None illum-profile=Flat na=1.4";
   struct Case { const char* name; const char* spec; std::vector<std::pair<int, double>> lasers; };
   const Case cases[] = {
      { "dSTORM AF647 at 60 s", "frames=8 mt-dye=AF647 light-preset=auto", { { 640, 2.0 } } },
      { "dSTORM AF647 from 0 (initial ON)", "frames=6 mt-dye=AF647 light-preset=auto start-sec=0", { { 640, 2.0 } } },
      { "PALM mEos3.2 + 405 nm, pre state", "frames=6 mt-dye=mEos3.2 light-preset=auto laser-405=0.05", { { 561, 1.0 }, { 405, 0.05 } } },
      { "WideField mEGFP, mean field", "frames=3 mt-dye=mEGFP light-preset=auto", { { 488, 0.01 } } },
      { "WideField mEGFP, per dye", "frames=4 mt-dye=mEGFP light-preset=auto mt-label-pct=3 mean-field-density-per-um2=1e9 mean-field-max-emitters=1e9 start-sec=0 mt-dye.photon-budget=2000", { { 488, 0.01 } } },
      { "DNA-PAINT ATTO 655 + imager", "frames=6 chamber-height-um=5", { { 640, 1.0 } } },
   };
   for (const Case& c : cases)
   {
      const auto t0 = std::chrono::steady_clock::now();
      std::string scaled;
      for (const auto& l : c.lasers)
      {
         std::snprintf(b, sizeof b, " laser-%d=%.17g", l.first, l.second * F);
         scaled += b;
      }
      bool ok1 = false, ok2 = false;
      const std::vector<uint16_t> a = Movie(B + " " + c.spec + " illum-bfp=0.6", ok1);
      const std::vector<uint16_t> e = Movie(B + " " + c.spec + " illum-bfp=0" + scaled, ok2);
      const double frac = Identical(a, e);
      std::snprintf(b, sizeof b, "uniform F = %.3f vs lasers x F, %s: %.2f%% identical ADU (%.0f ms)", F, c.name,
                    100 * frac, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      Check(ok1 && ok2 && frac >= 0.995, b);
   }
   {
      bool ok1 = false, ok2 = false;
      const std::string D = "world-seed=1249 x=63 y=3 size=32 frames=3 psf-model=0 background-per-sec=0";
      const std::vector<uint16_t> clipped = Movie(D + " na=1.4 illum-bfp=1.1", ok1);
      const std::vector<uint16_t> none = Movie(D + " mt-label-pct=0 npc-dye=None", ok2);
      Check(ok1 && ok2 && Identical(clipped, none) == 1.0, "a clipped spot (NA 1.40, bfp 1.1) = a sample without light");
   }
   {
      bool ok1 = false, ok2 = false, ok3 = false;
      const std::string R = "specimen=Nanorulers world-seed=1249 x=3 y=3 size=128 frames=1 psf-model=0 background-per-sec=0";
      const double dark = Mean(Movie(R + " light-epi=0", ok3));
      const double epi = Mean(Movie(R + " illum-bfp=0", ok1)) - dark, tirf = Mean(Movie(R, ok2)) - dark;
      std::snprintf(b, sizeof b, "nanorulers DNA-PAINT, mean ADU over dark: epi %.1f, TIRF (the default) %.2f", epi, tirf);
      Check(ok1 && ok2 && ok3 && tirf < 0.1 * epi, b);
   }
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   Units();
   Movies();
   std::printf("excitation check: %.1f s, %s\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
               g_failures ? "FAILED" : "PASS");
   return g_failures ? 1 : 0;
}
