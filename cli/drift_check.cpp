// ctest drift: the sample drift (Simulation/Drift.h): random walk and directed part.
//   - statistics: Var d(t) = sigma^2 t per axis whatever the frame time, mean 0, axes uncorrelated;
//   - live mode's step-by-step sum = the stack's trajectory, bit for bit;
//   - SuperRes: frame f of a drifting movie = the movie posed where the drift put the sample at f.
//   - directed part: constant velocity exact; random direction per seed uniform; the direction and speed wanders
//     have the set RMS and correlation time; z keeps its sign; random-walk-only paths unchanged by it.
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

// Velocity of frame f (f >= 1) of a path: (d(f+1) - d(f)) / dt.
void Velocity(const std::vector<DriftNm>& t, size_t f, double dt, double& vx, double& vy, double& vz)
{
   vx = (t[f + 1].x - t[f].x) / dt;
   vy = (t[f + 1].y - t[f].y) / dt;
   vz = (t[f + 1].z - t[f].z) / dt;
}

void Directed()
{
   const double dt = 0.05, kPi = 3.14159265358979323846;
   // Constant velocity: no wander, no random walk.
   {
      DriftSettings s;
      s.xySpeedNmPerSec = 50.0;
      s.xyAngleDeg = 30.0;
      s.zSpeedNmPerSec = 8.0;
      s.zDirection = -1;
      s.angleWanderDeg = 0.0;
      s.zAngleWanderDeg = 0.0;
      const std::vector<DriftNm> t = DriftTrajectory(7, 201, dt, s);
      const double T = 200 * dt;
      const double ex = 50.0 * std::cos(kPi / 6) * T, ey = 50.0 * std::sin(kPi / 6) * T, ez = -8.0 * T;
      const bool ok = std::fabs(t[200].x - ex) < 1e-9 && std::fabs(t[200].y - ey) < 1e-9 && std::fabs(t[200].z - ez) < 1e-9;
      char msg[160];
      std::snprintf(msg, sizeof msg, "constant velocity: d(10 s) = (%.4f, %.4f, %.4f) nm = 10 s x (50 nm/s at 30 deg, -8 nm/s)",
                    t[200].x, t[200].y, t[200].z);
      Check(ok, msg);
   }
   // Random direction per seed: xy uniform (mean resultant length ~ 1/sqrt(n)), z up for half the seeds.
   {
      DriftSettings s;
      s.xySpeedNmPerSec = 10.0;
      s.zSpeedNmPerSec = 10.0;
      s.angleWanderDeg = 0.0;
      s.zAngleWanderDeg = 0.0;
      double c = 0, sn = 0;
      int up = 0;
      const int n = 2000;
      for (int seed = 1; seed <= n; seed++) {
         const std::vector<DriftNm> t = DriftTrajectory(seed, 2, 1.0, s);
         c += t[1].x / 10.0;
         sn += t[1].y / 10.0;
         up += t[1].z > 0.0 ? 1 : 0;
      }
      const double R = std::sqrt(c * c + sn * sn) / n, fUp = static_cast<double>(up) / n;
      char msg[160];
      std::snprintf(msg, sizeof msg,
                    "random direction per seed: xy mean resultant length %.3f over %d seeds (uniform: ~%.3f), z up %.3f (0.5)",
                    R, n, 1.0 / std::sqrt(n), fUp);
      Check(R < 4.0 / std::sqrt(n) && std::fabs(fUp - 0.5) < 4.0 * 0.5 / std::sqrt(n), msg);
   }
   // Wanders: the xy direction swings within +/- 30 deg (uniform: RMS 30 / sqrt 3, correlation after tau (6 / pi)
   // asin(1 / 2e) = 0.353), speed RMS 30%, correlation time 5 s; z up with a 90 deg swing: speed x cos, never back,
   // mean cos 2 / pi.
   {
      DriftSettings s;
      s.xySpeedNmPerSec = 40.0;
      s.xyAngleDeg = 90.0;
      s.zSpeedNmPerSec = 10.0;
      s.zDirection = 1;
      s.angleWanderDeg = 30.0;
      s.zAngleWanderDeg = 90.0;
      s.speedWanderPct = 30.0;
      s.wanderTimeSec = 5.0;
      const int n = 2000, lag = 100;   // 100 frames = 5 s = tau
      double a2 = 0, aa = 0, amax = 0, sp = 0, sp2 = 0, zc = 0;
      bool zSign = true;
      for (int seed = 1; seed <= n; seed++) {
         const std::vector<DriftNm> t = DriftTrajectory(seed, 50 + lag + 2, dt, s);
         double vx, vy, vz, wx, wy, wz;
         Velocity(t, 50, dt, vx, vy, vz);
         Velocity(t, 50 + lag, dt, wx, wy, wz);
         const double a = std::atan2(vy, vx) - kPi / 2, b = std::atan2(wy, wx) - kPi / 2;
         a2 += a * a;
         aa += a * b;
         amax = std::max(amax, std::fabs(a));
         const double v = std::sqrt(vx * vx + vy * vy) / 40.0;
         sp += v;
         sp2 += (v - 1.0) * (v - 1.0);
         zc += vz / 10.0;
         zSign = zSign && vz >= 0.0 && wz >= 0.0;
      }
      const double rmsDeg = std::sqrt(a2 / n) * 180 / kPi, corr = aa / a2, mean = sp / n, rmsSpeed = std::sqrt(sp2 / n);
      const double maxDeg = amax * 180 / kPi, zMean = zc / n, swingCorr = 6.0 / kPi * std::asin(std::exp(-1.0) / 2.0);
      char msg[300];
      std::snprintf(msg, sizeof msg,
                    "wander: direction RMS %.1f deg (%.1f), max %.1f (<= 30), correlation after tau %.3f (%.3f), speed mean "
                    "%.3f (~1) RMS %.3f (0.30), z mean %.3f (2/pi = 0.637) and never back",
                    rmsDeg, 30.0 / std::sqrt(3.0), maxDeg, corr, swingCorr, mean, rmsSpeed, zMean);
      Check(std::fabs(rmsDeg / (30.0 / std::sqrt(3.0)) - 1) < 0.07 && maxDeg <= 30.0 + 1e-6 &&
            std::fabs(corr - swingCorr) < 0.06 && std::fabs(mean - 1) < 0.03 && std::fabs(rmsSpeed / 0.3 - 1) < 0.1 &&
            std::fabs(zMean / (2.0 / kPi) - 1) < 0.05 && zSign, msg);
   }
   // A 180 deg z swing also reverses: some seeds move down although the direction is up.
   {
      DriftSettings s;
      s.zSpeedNmPerSec = 10.0;
      s.zDirection = 1;
      s.zAngleWanderDeg = 180.0;
      int down = 0;
      const int n = 400;
      for (int seed = 1; seed <= n; seed++) {
         const std::vector<DriftNm> t = DriftTrajectory(seed, 2, dt, s);
         down += t[1].z < 0.0 ? 1 : 0;
      }
      char msg[120];
      std::snprintf(msg, sizeof msg, "z swing 180 deg: %d of %d seeds start downwards (~half)", down, n);
      Check(down > n / 4 && down < 3 * n / 4, msg);
   }
   // The swing: erf(x / sqrt 2) within 1.5e-7 (A&S 7.1.26) at a few points (2 Phi(x) - 1).
   {
      const double xs[] = { 0.0, 0.5, 1.0, 2.0, -1.5 };
      const double ref[] = { 0.0, 0.38292492254802624, 0.6826894921370859, 0.9544997361036416, -0.8663855974622838 };
      double worst = 0.0;
      for (int i = 0; i < 5; i++)
         worst = std::max(worst, std::fabs(DriftSwing(xs[i]) - ref[i]));
      char msg[120];
      std::snprintf(msg, sizeof msg, "swing = erf(x / sqrt 2) within %.2g (< 1.5e-7)", worst);
      Check(worst < 1.5e-7, msg);
   }
   // The directed part does not move the random walk's draws: with speeds 0 the path is the step sum.
   {
      DriftSettings s;
      s.xyNmPerSqrtSec = 13.0;
      s.zNmPerSqrtSec = 31.0;
      s.angleWanderDeg = 40.0;
      s.speedWanderPct = 50.0;
      const std::vector<DriftNm> t = DriftTrajectory(42, 300, dt, s);
      DriftNm d;
      bool same = true;
      for (long f = 1; f < 300; f++) {
         const DriftNm st = DriftStep(DriftSeed(42), f, dt, s);
         d.x += st.x;
         d.y += st.y;
         d.z += st.z;
         same = same && d.x == t[f].x && d.y == t[f].y && d.z == t[f].z;
      }
      Check(same, "speeds 0: the random-walk path is unchanged, bit for bit (wander settings alone do nothing)");
   }
}

} // namespace

int main()
{
   Directed();
   Statistics(0.01);
   Statistics(0.1);
   LiveSum();
   SuperRes();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall drift checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
