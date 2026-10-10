// ctest cli_probes: the cli's diagnostic outputs (scope_probes.h, ScopeResolved.h) show what a movie renders.
//   - RenderScopePhotons: an ideal camera's ADU are the photons plus shot noise only; a BrightField empty field is
//     the lamp's flux everywhere; no light gives zeros; both lights = fluorescence + lamp x QE at the lamp;
//   - ScopeResolve: the label states equal ScopeLabelState's;
//   - the splat of one blink sums to 1 with the whole kernel, less with the halo cut (by less than the cut per
//     left-out pixel);
//   - the PSF, dye and preset outputs are written, and Renderer.Quality's Realistic preset is the option defaults;
//   - --history-before: a past equal to the movie's settings changes nothing, a power step acts from then on.
//
//   probes_check          exit code 0 = all checks passed (writes probes_check_* files in the working directory)
#include "RenderPresets.h"
#include "ScopeResolved.h"
#include "scope_probes.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok) g_failures++;
}

ScopeSpec Spec(const char* text)
{
   ScopeSpec s;
   std::string err;
   if (!ParseScopeSpec(text, s, err))
   {
      std::printf("FAIL  spec %s: %s\n", text, err.c_str());
      std::exit(1);
   }
   return s;
}

// Every frame's photons (concatenated) and ADU of a spec.
void Movie(const ScopeSpec& s, std::vector<float>& photons, std::vector<double>& adu)
{
   ScopeMovieInfo info;
   std::string err;
   photons.clear();
   adu.clear();
   const bool a = RenderScopePhotons(s, [&](long, const std::vector<float>& p) {
      photons.insert(photons.end(), p.begin(), p.end());
      return true;
   }, info, err);
   const bool b = RenderScopeMovie(s, [&](long, const std::vector<uint16_t>& f) {
      adu.insert(adu.end(), f.begin(), f.end());
      return true;
   }, info, err);
   if (!a || !b)
      std::printf("FAIL  movie: %s\n", err.c_str());
}

std::string ReadText(const std::string& path)
{
   std::ifstream f(path, std::ios::binary);
   std::stringstream t;
   t << f.rdbuf();
   return t.str();
}

// The number after "key": in a JSON text (NaN if absent).
double JsonNumber(const std::string& text, const char* key)
{
   const std::string k = std::string("\"") + key + "\":";
   const size_t i = text.find(k);
   return i == std::string::npos ? NAN : std::atof(text.c_str() + i + k.size());
}

const char* kIdeal = "gain=1 read-noise=0 read-noise-std-pct=0 gain-std-pct=0 offset=0 offset-std=0 dark-per-sec=0";

} // namespace

int main()
{
   char msg[256];
   const std::string base = std::string("size=32 x=-4 y=-5 disk-cache=0 psf-kernel-half-width-nm=2000 psf-z-range-um=2 ") +
                            kIdeal;

   // Fluorescence: ADU (gain 1, no read noise or offset, QE 1 in the chain) = Poisson(photons).
   {
      std::vector<float> ph;
      std::vector<double> adu;
      Movie(Spec((base + " frames=4").c_str()), ph, adu);
      double sp = 0, sa = 0;
      for (float v : ph) sp += v;
      for (double v : adu) sa += v;
      std::snprintf(msg, sizeof msg, "fluorescence: %zu photon pixels = %zu ADU pixels, sum %.0f photons vs %.0f ADU",
                    ph.size(), adu.size(), sp, sa);
      Check(ph.size() == adu.size() && ph.size() == 32u * 32u * 4u && sp > 1000 &&
            std::fabs(sa - sp) < 5 * std::sqrt(sp) + 1, msg);
   }
   // BrightField, an empty field (no cells): the lamp's flux in every pixel.
   std::vector<float> bf, fl, both, dark;
   std::vector<double> adu;
   {
      Movie(Spec((base + " frames=1 occupancy=0 modality=BrightField bf-quality=1").c_str()), bf, adu);
      const double flux = 80000 * 0.05;
      double worst = 0;
      for (float v : bf) worst = std::max(worst, std::fabs(v / flux - 1));
      std::snprintf(msg, sizeof msg, "BrightField empty field: photons / flux - 1 at most %.2g", worst);
      Check(!bf.empty() && worst < 1e-3, msg);
   }
   // No light: zeros. Both lights: fluorescence + lamp x the QE at the lamp wavelength.
   {
      const std::string s = base + " frames=2 bf-quality=1";
      Movie(Spec((s + " light-epi=0 light-trans=0").c_str()), dark, adu);
      bool zero = !dark.empty();
      for (float v : dark) zero = zero && v == 0.0f;
      Check(zero, "no light: zero photons");
      std::vector<float> lamp;
      Movie(Spec((s + " light-epi=1 light-trans=0").c_str()), fl, adu);
      Movie(Spec((s + " light-epi=0 light-trans=1").c_str()), lamp, adu);
      Movie(Spec((s + " light-epi=1 light-trans=1").c_str()), both, adu);
      ScopeResolved R;
      std::string err;
      ScopeResolve(Spec(s.c_str()), R, err);
      const double qe = SampleAt(R.lp.qe, 550.0);
      double worst = 0;
      for (size_t i = 0; i < both.size() && i < fl.size() && i < lamp.size(); ++i)
         worst = std::max(worst, std::fabs(both[i] - (fl[i] + lamp[i] * qe)) / (1 + std::fabs(both[i])));
      std::snprintf(msg, sizeof msg, "both lights = fluorescence + lamp x QE %.3f: largest relative difference %.2g", qe,
                    worst);
      Check(both.size() == fl.size() && both.size() == lamp.size() && !both.empty() && worst < 1e-5, msg);
   }
   // ScopeResolve's label states = ScopeLabelState's.
   {
      ScopeResolved R;
      ScopeStateReadout st;
      std::string err;
      const ScopeSpec s = Spec(base.c_str());
      const bool ok = ScopeResolve(s, R, err) && ScopeLabelState(s, 0, false, st, err) && !R.labels.empty();
      std::snprintf(msg, sizeof msg, "ScopeResolve: detected fraction %.4f, %.1f nm, %.0f photons/s (ScopeLabelState: %.4f)",
                    ok ? R.labels[0].main.detectedFraction : -1, ok ? R.labels[0].main.lambdaNm : -1,
                    ok ? R.labels[0].main.detectedPerSec : -1, st.detectedFraction);
      Check(ok && R.labels[0].main.detectedFraction == st.detectedFraction && st.detectedFraction > 0 &&
            st.detectedFraction < 1 && R.labels[0].main.detectedPerSec == st.detectedPerSec, msg);
   }
   // One blink: the whole kernel sums to 1; the halo cut leaves out a little (the default 7 um kernel).
   {
      std::string err;
      const std::string s = "disk-cache=0 psf-z-range-um=1 ";
      const bool a = probes::Splat(Spec((s + "psf-halo-cut=0").c_str()), 0.3, -0.2, 0.0, "probes_check_splat0.tif", err);
      const bool b = probes::Splat(Spec((s + "psf-halo-cut=3e-6").c_str()), 0.3, -0.2, 0.0, "probes_check_splat.tif", err);
      const std::string j0 = ReadText("probes_check_splat0.tif.json"), j = ReadText("probes_check_splat.tif.json");
      const double s0 = JsonNumber(j0, "sum"), s1 = JsonNumber(j, "sum"), kept = JsonNumber(j, "kept_pixels"),
                   square = JsonNumber(j, "square_pixels");
      // Every left-out pixel would have had less than the cut: the missing light is at most that per pixel.
      std::snprintf(msg, sizeof msg,
                    "splat: whole kernel sums to %.7f, halo cut 3e-6 to %.7f keeping %.0f of %.0f pixels (missing %.2g <= %.2g)",
                    s0, s1, kept, square, s0 - s1, (square - kept) * 3e-6);
      Check(a && b && std::fabs(s0 - 1) < 1e-5 && s1 < s0 && kept < square && s0 - s1 <= (square - kept) * 3e-6, msg);
   }
   // PSF, dyes, presets: written; the PSF planes and camera images of a blink each sum to 1.
   {
      std::string err;
      const ScopeSpec s = Spec(base.c_str());
      ScopePsfPreview pv;
      bool sums = MakeScopePsfPreview(s, pv, err) && pv.nz > 0;
      for (int z = 0; sums && z < pv.nz; ++z)
      {
         double a = 0, c = 0;
         for (int i = 0; i < pv.size * pv.size; ++i) a += pv.planes[static_cast<size_t>(z) * pv.size * pv.size + i];
         for (int i = 0; i < pv.camSize * pv.camSize; ++i) c += pv.cams[static_cast<size_t>(z) * pv.camSize * pv.camSize + i];
         sums = std::fabs(a - 1) < 1e-4 && std::fabs(c - 1) < 1e-4;
      }
      Check(sums && probes::Psf(s, "probes_check_psf", err), "PSF: planes and camera images sum to 1, files written");
      const double rect[4] = { -5.6, -6.6, -2.4, -3.4 };
      const bool d = probes::DyesJson(s, rect, -1e9, 1e9, 60, 61, "probes_check_dyes.json", err);
      const std::string dj = ReadText("probes_check_dyes.json");
      const size_t at = dj.find("\"sites\":[");
      Check(d && at != std::string::npos && dj[at + 9] != ']', "dyes: sites in the FOV");
      Check(probes::PresetsJson("probes_check_presets.json", err) &&
            ReadText("probes_check_presets.json").find("\"Realistic\"") != std::string::npos, "presets written");
   }
   // --history-before (a two-epoch rate history, MakePastClock): a past equal to the movie's own settings changes
   // nothing; WideField with 4x the 488 nm power from clock 30 s on is 4x as bright as the unchanged field at 30 s
   // (the dyes bleached so far stay bleached), and brighter than 4x power read from clock 0 (more of them bleached).
   {
      auto total = [&](const ScopeSpec& s, const ScopeSpec* past) {
         std::unique_ptr<DyeClock> clock;
         if (past)
            clock = probes::MakePastClock(*past, s);
         ScopeMovieInfo info;
         std::string err;
         double sum = 0;
         if (!RenderScopePhotons(s, [&](long, const std::vector<float>& p) {
                for (float v : p) sum += v;
                return true;
             }, info, err, clock.get()))
            std::printf("FAIL  history movie: %s\n", err.c_str());
         return sum;
      };
      const std::string wf = base + " frames=1 start-sec=30 mt-mode=WideField mt-dye=-1 light-preset=auto";
      const ScopeSpec p1 = Spec((wf + " laser-488=0.01").c_str()), p4 = Spec((wf + " laser-488=0.04").c_str());
      const double unchanged = total(p1, nullptr), same = total(p1, &p1), stepped = total(p4, &p1),
                   reread = total(p4, nullptr);
      std::snprintf(msg, sizeof msg, "history: same past %.6g = no history %.6g; 488 nm x4 at 30 s: %.3fx the unchanged "
                    "field (4x power from 0: %.3fx)", same, unchanged, stepped / unchanged, reread / unchanged);
      Check(unchanged > 0 && std::fabs(same / unchanged - 1) < 1e-5 && std::fabs(stepped / unchanged - 4) < 0.1 &&
            reread / unchanged < 3, msg);
   }
   // Renderer.Quality's Realistic = the cli/viewer option defaults (one table, RenderPresets.h).
   {
      auto def = [](const char* n) { return ScopeSpecGet(ScopeSpec(), n); };
      const QualityPreset& r = kQualityPresets[1];
      std::snprintf(msg, sizeof msg, "Quality %s = defaults: bf-quality %g, psf-oversampling %g, wf-upscale %g, psf-halo-cut %g",
                    r.name, def("bf-quality"), def("psf-oversampling"), def("wf-upscale"), def("psf-halo-cut"));
      Check(!std::strcmp(r.name, "Realistic") && def("bf-quality") == r.bf && def("psf-oversampling") == r.os &&
            def("wf-upscale") == r.wf && def("psf-halo-cut") == r.halo, msg);
   }
   std::printf(g_failures ? "%d check(s) FAILED\n" : "all checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
