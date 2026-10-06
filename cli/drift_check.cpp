// ctest drift: the random-walk sample drift (Simulation/Drift.h).
//   - statistics: Var d(t) = sigma^2 t per axis whatever the frame time, mean 0, axes uncorrelated;
//   - live mode's step-by-step sum = the stack's trajectory, bit for bit;
//   - SuperRes: frame f of a drifting movie = the movie posed where the drift put the sample at f.
// WideField and BrightField drift: ctest widefield / brightfield (shift and focus-grid checks).
//
//   drift_check          exit code 0 = all checks passed
#include "Drift.h"
#include "ScopeMovie.h"

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
   if (!ok) g_failures++;
}

// d at t = 1 s over many seeds, frames of frameSec.
void Statistics(double frameSec)
{
   DriftSettings s;
   s.xyNmPerSqrtSec = 20.0;
   s.zNmPerSqrtSec = 7.0;
   const long frames = static_cast<long>(std::lround(1.0 / frameSec)) + 1;
   const int n = 4000;
   double m[3] = {0, 0, 0}, v[3] = {0, 0, 0}, cxy = 0, cxz = 0;
   for (int seed = 1; seed <= n; seed++) {
      const DriftNm d = DriftTrajectory(seed, frames, frameSec, s).back();
      const double a[3] = {d.x, d.y, d.z};
      for (int k = 0; k < 3; k++) { m[k] += a[k]; v[k] += a[k] * a[k]; }
      cxy += d.x * d.y;
      cxz += d.x * d.z;
   }
   for (int k = 0; k < 3; k++) { m[k] /= n; v[k] = v[k] / n - m[k] * m[k]; }
   const double want[3] = {400.0, 400.0, 49.0};
   bool ok = true;
   for (int k = 0; k < 3; k++)
      ok = ok && std::fabs(v[k] / want[k] - 1.0) < 0.07 && std::fabs(m[k]) < 4.0 * std::sqrt(want[k] / n);
   const double rxy = cxy / n / std::sqrt(v[0] * v[1]), rxz = cxz / n / std::sqrt(v[0] * v[2]);
   ok = ok && std::fabs(rxy) < 0.06 && std::fabs(rxz) < 0.06;
   char msg[200];
   std::snprintf(msg, sizeof msg,
                 "%g ms frames: Var d(1 s) = %.1f, %.1f, %.1f nm^2 (want 400, 400, 49), mean %.2f %.2f %.2f, r_xy %.3f r_xz %.3f",
                 frameSec * 1000, v[0], v[1], v[2], m[0], m[1], m[2], rxy, rxz);
   Check(ok, msg);
}

void LiveSum()
{
   DriftSettings s;
   s.xyNmPerSqrtSec = 13.0;
   s.zNmPerSqrtSec = 31.0;
   const std::vector<DriftNm> t = DriftTrajectory(42, 500, 0.03, s);
   DriftNm d;
   bool same = t[0].x == 0 && t[0].y == 0 && t[0].z == 0;
   for (long f = 1; f < 500; f++) {
      const DriftNm st = DriftStep(DriftSeed(42), f, 0.03, s);
      d.x += st.x;
      d.y += st.y;
      d.z += st.z;
      same = same && d.x == t[f].x && d.y == t[f].y && d.z == t[f].z;
   }
   Check(same, "live mode's step sum = the stack's trajectory, bit for bit");
   DriftSettings off;
   const std::vector<DriftNm> z = DriftTrajectory(42, 50, 0.03, off);
   bool zero = true;
   for (const DriftNm& p : z) zero = zero && p.x == 0 && p.y == 0 && p.z == 0;
   Check(zero, "no drift: zero trajectory");
}

std::vector<uint16_t> Movie(const std::string& text, ScopeMovieInfo& info)
{
   ScopeSpec spec;
   std::string err;
   std::vector<uint16_t> all;
   if (!ParseScopeSpec(text, spec, err) ||
       !RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& a) { all.insert(all.end(), a.begin(), a.end()); return true; },
                         info, err))
      std::printf("  %s: %s\n", text.c_str(), err.c_str());
   return all;
}

// SR: frame f of the drifting movie against a still movie posed at x - dx(f), y - dy(f), z - dz(f) (every dye:
// z-range-um=0, so the cull windows cannot differ; a kernel inside the 2 um query margin, so the query rects
// cannot either).
void SuperRes()
{
   const std::string base = "world-seed=1249 size=48 psf-kernel-half-width-nm=1500 z-range-um=0 ";
   const double x = 63, y = 3, z = 0.5;
   char drifting[256];
   std::snprintf(drifting, sizeof drifting, "%sx=%g y=%g z=%g frames=30 drift-xy-nm-per-sqrt-sec=200 drift-z-nm-per-sqrt-sec=300",
                 base.c_str(), x, y, z);
   ScopeMovieInfo info;
   const std::vector<uint16_t> a = Movie(drifting, info);
   const size_t px = 48 * 48;
   bool ok = a.size() == 30 * px && info.driftNm.size() == 90;
   double worst = 1.0;
   for (long f : {12L, 29L}) {
      if (!ok) break;
      const double dx = info.driftNm[3 * f], dy = info.driftNm[3 * f + 1], dz = info.driftNm[3 * f + 2];
      char still[256];
      std::snprintf(still, sizeof still, "%sx=%.9f y=%.9f z=%.9f frames=%ld", base.c_str(), x - dx / 1000, y - dy / 1000,
                    z - dz / 1000, f + 1);
      ScopeMovieInfo bi;
      const std::vector<uint16_t> b = Movie(still, bi);
      if (b.size() != (f + 1) * px) { ok = false; break; }
      size_t same = 0;
      for (size_t i = 0; i < px; i++) same += a[f * px + i] == b[f * px + i];
      worst = std::min(worst, static_cast<double>(same) / px);
      std::printf("      frame %ld: drift (%.1f, %.1f, %.1f) nm, %.3f%% of pixels identical to the posed movie\n", f, dx, dy, dz,
                  100.0 * same / px);
   }
   Check(ok && worst >= 0.999, "SuperRes: a drifting frame = the movie posed at the drifted sample (>= 99.9% identical ADU)");
   // The default (no drift) gives no trajectory.
   ScopeMovieInfo ni;
   Movie(base + "x=63 y=3 frames=2", ni);
   Check(ni.driftNm.empty(), "no drift: no trajectory in the movie info");
}

} // namespace

int main()
{
   Statistics(0.01);
   Statistics(0.1);
   LiveSum();
   SuperRes();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall drift checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
