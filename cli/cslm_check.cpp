///////////////////////////////////////////////////////////////////////////////
// FILE:          cslm_check.cpp
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   ctest `cslm`: confocal laser scanning excitation (ScanSpot.h;
//                spec/ALGORITHM.md "Confocal laser scanning (CSLM)
//                excitation").
//                  Spot:     a low-NA focus = the Airy disc (peak per power
//                            pi NA^2 / lambda^2, FWHM 0.5145 lambda / NA) and
//                            its paraxial axial FWHM; every plane one unit of
//                            power (a whole scan's interior = 1 at any z).
//                  Windows:  the runs a frame's exposure takes = the sweep cut
//                            into short time steps (line by line, flyback dark,
//                            across the scan's end); q frames of T / q add up
//                            to q x the mean; the periods; a band lights its
//                            lines and not the others.
//                  Excitation: the scan as the Excitation's light: At = the
//                            scan light's; MeanAt (the volume) = the analytic
//                            mean; the prepared plane = PlaneMean; a bead's
//                            ratio is the frame's over the mean.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BfpField.h"
#include "Excitation.h"
#include "ScanSpot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_fail = 0;
constexpr double kPi = 3.14159265358979323846;

void Check(bool ok, const std::string& what)
{
   std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok)
      ++g_fail;
}

std::string F(const char* fmt, double a, double b = 0, double c = 0)
{
   char buf[256];
   std::snprintf(buf, sizeof buf, fmt, a, b, c);
   return buf;
}

std::shared_ptr<const BfpLight> Make(double na, double fill, double fieldUm, double pixelUm, double dwellUs, double flybackUs,
                                     double nm = 500)
{
   ScanLightParams p;
   p.lines = { nm };
   p.lambdaRefNm = nm;
   p.na = na;
   p.nS = 1.337;
   p.fieldUm = fieldUm;
   p.pixelUm = pixelUm;
   p.dwellSec = dwellUs * 1e-6;
   p.flybackSec = flybackUs * 1e-6;
   p.fill = fill;
   p.zSampleUm = 12;
   std::string err;
   auto L = MakeScanLight(p, err);
   if (!L)
      std::printf("MakeScanLight: %s\n", err.c_str());
   return L;
}

void Spot()
{
   // A low NA: the scalar Debye focus is the Airy disc to ~1 % (sqrt(cos) and the sine condition at 13 degrees).
   const double nm = 500, na = 0.3, lam = nm * 1e-3;
   FocusedSpot s;
   s.Make(nm, na, 1.337, 12);
   const double airyPeak = kPi * na * na / (lam * lam);
   Check(std::fabs(s.PeakPerUm2() / airyPeak - 1.0) < 0.02, F("low-NA peak %.5g per um^2 = Airy %.5g", s.PeakPerUm2(), airyPeak));
   const double fw = 0.5145 * lam / na;
   Check(std::fabs(s.FwhmLateralUm() / fw - 1.0) < 0.02, F("low-NA lateral FWHM %.4f um = Airy %.4f", s.FwhmLateralUm(), fw));
   // Paraxial axial: sinc^2(k z (1 - cos t) / 2), half at 1.3916.
   const double k = 2 * kPi * 1.337 / lam, ct = std::sqrt(1 - std::pow(na / 1.337, 2));
   const double fa = 4 * 1.3916 / (k * (1 - ct));
   Check(std::fabs(s.FwhmAxialUm() / fa - 1.0) < 0.04, F("low-NA axial FWHM %.3f um = paraxial %.3f", s.FwhmAxialUm(), fa));
}

void Windows()
{
   // A small raster: 30 lines of 100 nm, 1 us dwell, 5 us flyback: line 35 us, scan 1.05 ms.
   auto L = Make(1.2, 1.0, 3.0, 0.1, 1.0, 5.0);
   const ScanLight& S = *L->Scan();
   const ScanProgram& g = S.Program();
   Check(g.N == 30 && std::fabs(g.scanSec - 1.05e-3) < 1e-12, F("raster: %g lines, scan %.6g s", g.N, g.scanSec));
   Check(ScanPeriodFrames(g.scanSec / 3, g.scanSec) == 3 && ScanPeriodFrames(2 * g.scanSec, g.scanSec) == 1 &&
            ScanPeriodFrames(g.scanSec * 0.3712345, g.scanSec) == 0,
         "periods: T / 3 -> 3, 2 T -> 1, an irrational share -> none");
   const FocusedSpot& sp = S.Spot(0);
   // A frame of 0.37 T (starting at frame 5: across the scan's end at some frames) = its sweep in 0.05 us steps.
   for (long long frame : { 2LL, 5LL })
   {
      const double E = 0.37 * g.scanSec;
      std::vector<ScanRun> runs;
      double norm;
      S.FrameRuns(frame, E, runs, norm);
      std::vector<ScanRun> steps;
      const double dt = 0.25e-6, t0 = frame * E;
      for (double t = t0, te; t < t0 + E - 1e-15; t = te)
      {
         // Steps of dt, cut at every line's start and the window's end.
         te = std::min({ t + dt, (std::floor(t / g.lineSec + 1e-9) + 1.0) * g.lineSec, t0 + E });
         const double s = std::max(0.0, t - std::floor(t / g.scanSec + 1e-9) * g.scanSec);
         const int k = std::min(g.N - 1, static_cast<int>(s / g.lineSec + 1e-9));
         const double u = std::max(0.0, s - k * g.lineSec);
         const double u1 = std::min(u + (te - t), g.sweepSec);
         if (u >= g.sweepSec)
            continue;   // the flyback: dark
         ScanRun r;
         r.k1 = r.k2 = k;
         r.xs = g.x0 + u * g.v;
         r.xe = g.x0 + u1 * g.v;
         steps.push_back(r);
      }
      double worst = 0, peak = 0;
      for (double zr : { 0.0, 0.8 })
         for (double y = -1.8; y <= 1.8; y += 0.13)
            for (double x = -1.8; x <= 1.8; x += 0.17)
            {
               const double a = sp.Sum(g, runs, x, y, zr), b = sp.Sum(g, steps, x, y, zr);
               worst = std::max(worst, std::fabs(a - b));
               peak = std::max(peak, std::fabs(b));
            }
      Check(worst <= 1e-4 * peak, F("frame %g: runs = the sweep in 0.25 us steps (max |diff| %.3g of %.4g)",
                                    static_cast<double>(frame), worst, peak));
   }
   // q frames of T / q add up to q x the mean.
   {
      std::vector<ScanRun> mr;
      double mnorm;
      S.MeanRuns(mr, mnorm);
      double worst = 0;
      for (double y : { -1.0, -0.33, 0.21, 1.4 })
         for (double x : { -1.2, 0.0, 0.77, 1.49 })
         {
            double sum = 0;
            for (int f = 0; f < 4; ++f)
            {
               std::vector<ScanRun> runs;
               double norm;
               S.FrameRuns(f, g.scanSec / 4, runs, norm);
               sum += S.At(0, runs, norm, x, y, 0.3);
            }
            const double m = S.At(0, mr, mnorm, x, y, 0.3);
            worst = std::max(worst, std::fabs(sum - 4 * m) / std::max(1e-9, 4 * m));
         }
      Check(worst < 1e-5, F("four frames of T / 4 = 4 x the mean (worst %.2g)", worst));
   }
}

void Interior()
{
   // A wide field at a high NA (capped at the medium): a whole scan's interior is the setting at every height the
   // spot's rays still fall within the field (at 3 um, 99 % of them within 27 um).
   auto L = Make(1.4, 1.0, 60.0, 0.1, 2.0, 10.0, 488);
   const ScanLight& S = *L->Scan();
   std::vector<ScanRun> runs;
   double norm;
   S.MeanRuns(runs, norm);
   double worst = 0;
   for (double zr : { 0.0, 0.25, 1.0, 3.0 })
      for (double x : { -3.0, 0.0, 2.17 })
         worst = std::max(worst, std::fabs(S.At(0, runs, norm, x, 0.43, zr) - 1.0));
   Check(worst < 3e-3, F("whole scan's interior = 1 at z 0..3 um (worst %.2g)", worst));
   // A frame of whole scans (E = 2 T) = the mean.
   S.FrameRuns(7, 2 * S.Program().scanSec, runs, norm);
   Check(std::fabs(S.At(0, runs, norm, 0.4, -1.1, 0.5) - 1.0) < 3e-3, "a frame of two whole scans = the mean");
   std::printf("      spot NA %.3f: FWHM %.0f nm lateral, %.0f nm axial; peak %.4g x the field mean; %s\n",
               1.337 * 0.999, S.Spot(0).FwhmLateralUm() * 1e3, S.Spot(0).FwhmAxialUm() * 1e3, S.PeakPerSetting(0),
               L->Info().c_str());
}

void Band()
{
   // A frame of 10 lines' time starting at line 10: lit there, dark three spot widths away.
   auto L = Make(1.2, 1.0, 6.0, 0.1, 1.0, 5.0);
   const ScanLight& S = *L->Scan();
   const ScanProgram& g = S.Program();
   const double E = 10 * g.lineSec;
   // frame F starts at F E = 10 lines: frame 1.
   std::vector<ScanRun> runs;
   double norm;
   S.FrameRuns(1, E, runs, norm);
   const double lit = S.At(0, runs, norm, 0.0, g.LineY(15), 0.0);
   const double near = S.At(0, runs, norm, 0.0, g.LineY(30), 0.0);
   const double far = S.At(0, runs, norm, 0.0, g.LineY(55), 0.0);
   // Inside the band the light is up to T / E x the mean (1): less the spot's line-spread tails beyond the band's 1 um.
   const double full = g.scanSec / E;
   Check(lit < full && lit > 0.85 * full, F("band: lit at %.4g of T / E = %.4g", lit / full, full));
   Check(near < 0.02 * lit && far == 0.0, F("band: 1.5 um away %.3g of it (the tails), 4 um away %.3g", near / lit, far));
}

void InExcitation()
{
   auto L = Make(1.2, 1.0, 8.0, 0.1, 1.0, 5.0, 561);
   ExcitationSpec es;
   es.na = 1.2;
   es.nMed = 1.337;
   es.nImm = 1.518;
   es.pivotUm = 1.0;
   es.fovSpanUm = 8.0;
   es.maps = L;
   es.lineNm = { 561 };
   es.lambdaRefNm = 561;
   es.expSec = 0.37 * L->Scan()->Program().scanSec;
   es.frame = 3;
   Excitation ex(es);
   const ExcitationShares sh = { { 561, 1.0 } };
   Check(ex.Mapped() && ex.Scan() && ex.Varies() && ex.PerFrameStates(), "an aperiodic exposure: every frame its own light");
   std::vector<ScanRun> runs;
   double norm;
   L->Scan()->FrameRuns(3, es.expSec, runs, norm);
   const double a = ex.At(0.3, -0.7, 1.2, sh), b = L->Scan()->At(0, runs, norm, 0.3, -0.7, 0.2);
   Check(std::fabs(a - b) <= 1e-12 * std::max(1.0, b), F("At = the scan light's (%.6g, %.6g)", a, b));
   // The mean volume against the analytic mean.
   std::vector<ScanRun> mr;
   double mnorm;
   L->Scan()->MeanRuns(mr, mnorm);
   double worst = 0;
   for (double zr : { -0.6, 0.0, 0.13, 2.0 })
      for (double y : { -4.1, -3.95, 0.0, 3.3 })
         for (double x : { -4.05, -2.0, 0.17, 3.98 })
            worst = std::max(worst, std::fabs(ex.MeanAt(x, y, 1.0 + zr, sh) - L->Scan()->At(0, mr, mnorm, x, y, zr)));
   Check(worst < 0.02, F("MeanAt (the volume) = the analytic mean within %.3g (2 %% of the interior)", worst));
   // The prepared plane = PlaneMean point by point.
   Excitation::PlaneMeanPrep prep;
   ex.PreparePlaneMean(1.05, 1.15, 0.05, sh, prep);
   worst = 0;
   double top = 0;
   for (double y = -4.5; y <= 4.5; y += 0.37)
      for (double x = -4.5; x <= 4.5; x += 0.29)
      {
         const double pm = ex.PlaneMean(x, y, 1.05, 1.15, 0.05, sh);
         worst = std::max(worst, std::fabs(ex.PlaneMeanAt(x, y, prep) - pm));
         top = std::max(top, pm);
      }
   Check(worst < 0.01 * top, F("prepared plane = PlaneMean within %.3g of %.4g", worst, top));
   // Whole scans: the mean, no per-frame light.
   es.expSec = 2 * L->Scan()->Program().scanSec;
   Excitation ex2(es);
   Check(!ex2.Varies() && ex2.Period() == 1, "an exposure of whole scans: the mean, no stepping");
   es.expSec = L->Scan()->Program().scanSec / 5;
   Excitation ex5(es);
   Check(ex5.Varies() && ex5.Period() == 5 && !ex5.PerFrameStates(), "T / 5: five states");
   // A bead: the frame's light over the mean's.
   const double r = 0.05;
   const double ratio = ex.AtBall(0.1, 0.2, 1.0, r, sh) / ex.DcAtBall(0.1, 0.2, 1.0, r, sh);
   Check(std::isfinite(ratio) && ratio >= 0, F("bead ratio frame / mean = %.4g", ratio));
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   Spot();
   Windows();
   Interior();
   Band();
   InExcitation();
   const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   std::printf("cslm: %s (%d failure%s), %.2f s\n", g_fail ? "FAILED" : "ok", g_fail, g_fail == 1 ? "" : "s", sec);
   return g_fail ? 1 : 0;
}
