///////////////////////////////////////////////////////////////////////////////
// FILE:          sim_pattern_check.cpp
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   ctest `sim_pattern`: structured illumination (SimPattern.h,
//                Excitation.h; spec/ALGORITHM.md "Structured illumination").
//                  Fringe:   two beams at +-u make a fringe of period lambda /
//                            (2 n_s u) in the medium, contrast 1 when
//                            s-polarized and |cos 2 theta_s| when p-polarized;
//                            TIRF-SIM (u > 1) decays as exp(-z / d).
//                  Fields:   the compiled terms (CompilePattern) equal the
//                            explicit field sum |sum_j E_j|^2 written out
//                            independently (the user's pattern_at_z /
//                            pattern_at_z_with_polarization3 in physical
//                            units), for every built-in type, polarization
//                            and a tilted, phased custom set.
//                  Cycle:    the pattern summed over a full phase cycle is
//                            nPhases x the cycle mean (MeanAt): what the
//                            kinetics run on.
//                  Orders:   every built-in type's orders separate
//                            (OrdersSeparate) and the matrix round-trips.
//                  Movie:    a SIM3D WideField movie's frames 5..14 equal a
//                            movie that starts at frame0 = 5 bit for bit; a
//                            mean-field SIM frame's fringe matches the per-dye
//                            rendering of the same dyes; the legacy beam is
//                            untouched (sim-type Off = no beam set).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Excitation.h"
#include "ScopeMovie.h"
#include "ScopeResolved.h"
#include "SimPattern.h"

#include <chrono>
#include <cmath>
#include <complex>
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
   if (!ok)
   {
      std::printf("FAIL: %s\n", what.c_str());
      ++g_fail;
   }
}

ExcitationSpec SpecFor(const BeamSet& set, int profile = ILLUM_FLAT)
{
   ExcitationSpec e;
   e.profile = profile;
   e.fwhmPct = 100;
   e.fovWidthUm = 12.8;
   e.na = 1.4;
   e.nImm = 1.518;
   e.nMed = 1.337;
   e.pivotUm = 0.5;
   e.beams = set;
   e.lambdaRefNm = 640;
   e.lineNm = { 640 };
   return e;
}

// The explicit field sum, written out from the beams (not from the compiled terms): every beam's lab-frame complex
// field vector x exp(i (k_par . r + kz z) + i phi) (evanescent: exp(-z / 2 d)), all summed, |.|^2; the envelopes left
// out (Flat).
double ExplicitPattern(const PatternState& st, double lambdaNm, double nS, double nI, double na, int polarization,
                       double x, double y, double z, bool forceS = false, bool forceP = false)
{
   const double k = 2.0 * kPi * nS / (lambdaNm * 1e-3);
   std::complex<double> Ex(0, 0), Ey(0, 0), Ez(0, 0);
   const std::complex<double> I(0, 1);
   for (const Beam& b : st.beams)
   {
      const BeamField f = BeamFieldOf(b, lambdaNm, nS, nI, na, polarization, forceS, forceP);
      if (f.clipped)
         continue;
      const double phi = b.AzimuthRad(), c = std::cos(phi), s = std::sin(phi);
      std::complex<double> ph = std::exp(I * (k * (b.ux * x + b.uy * y) + 2.0 * kPi * b.phaseTurns));
      if (f.evanescent)
         ph *= std::exp(-std::max(0.0, z) / (2.0 * f.decayUm));
      else
         ph *= std::exp(I * (k * f.kzOverK * z));
      Ex += (f.ex * c - f.ey * s) * ph;
      Ey += (f.ex * s + f.ey * c) * ph;
      Ez += f.ez * ph;
   }
   return std::norm(Ex) + std::norm(Ey) + std::norm(Ez);
}

void FringeChecks()
{
   for (int pol : { SIM_POL_S, SIM_POL_P })
   {
      const double u = 0.6;
      BeamSet set = MakeBeams(SIM_2D, u, pol, 1.0);
      set.angles = 1;
      Excitation ex(SpecFor(set));
      const ExcitationShares sh = { { 640.0, 1.0 } };
      const double period = 0.64 / (2.0 * 1.337 * u);
      // Along x: the pattern at x and x + period agree; the contrast (max - min) / (max + min).
      double mx = 0, mn = 1e300;
      for (int i = 0; i < 400; ++i)
      {
         const double x = i * period / 400.0;
         const double v = ex.At(x, 0.0, 0.5, sh);
         mx = std::max(mx, v);
         mn = std::min(mn, v);
         Check(std::fabs(ex.At(x + period, 0.0, 0.5, sh) - v) < 1e-9 * std::max(1.0, mx),
               "two beams: the period lambda / (2 n_s u) at x " + std::to_string(x) + ": " +
                  std::to_string(ex.At(x + period, 0.0, 0.5, sh)) + " vs " + std::to_string(v));
      }
      const double contrast = (mx - mn) / (mx + mn);
      const double cosS = std::sqrt(1.0 - u * u), expect = pol == SIM_POL_S ? 1.0 : std::fabs(2.0 * cosS * cosS - 1.0);
      Check(std::fabs(contrast - expect) < 1e-9, "two beams: contrast " + std::to_string(contrast) + " expected " +
                                                    std::to_string(expect) + (pol == SIM_POL_S ? " (s)" : " (p)"));
      // Along y (across the fringe) and in z (two propagating beams at the same angle): constant.
      Check(std::fabs(ex.At(0.1, 3.0, 0.5, sh) - ex.At(0.1, 0.0, 0.5, sh)) < 1e-9 * mx, "two beams: constant along y");
      Check(std::fabs(ex.At(0.1, 0.0, 2.5, sh) - ex.At(0.1, 0.0, 0.5, sh)) < 1e-9 * mx, "two beams: constant in z");
      // The periods readout.
      const std::vector<double> per = ex.PeriodsUm(640);
      Check(per.size() == 1 && std::fabs(per[0] - period) < 1e-12, "PeriodsUm");
   }
   // TIRF-SIM: beams beyond the critical angle decay as exp(-z / d), the fringe stays.
   {
      const double u = 1.03;
      BeamSet set = MakeBeams(SIM_2D, u, SIM_POL_S, 1.0);
      set.angles = 1;
      Excitation ex(SpecFor(set));
      const ExcitationShares sh = { { 640.0, 1.0 } };
      const double d = ex.ComponentDecayUm(0, 640);
      Check(ex.Tirf() && ex.Regime() == REGIME_TIRF, "TIRF-SIM regime");
      for (double x : { 0.0, 0.07, 0.13 })
      {
         const double a = ex.At(x, 0.0, 0.0, sh), b = ex.At(x, 0.0, 0.2, sh);
         Check(std::fabs(b - a * std::exp(-0.2 / d)) < 1e-9 * a + 1e-12, "TIRF-SIM: exp(-z / d)");
      }
      const double period = 0.64 / (2.0 * 1.337 * u);
      Check(std::fabs(ex.At(0.3 + period, 0.0, 0.05, sh) - ex.At(0.3, 0.0, 0.05, sh)) < 1e-9, "TIRF-SIM: the period");
   }
   // The legacy beam: no beam set, the single beam's old factor.
   {
      ExcitationSpec e = SpecFor(BeamSet(), ILLUM_GAUSSIAN);
      e.bfp = 0.95;
      Excitation ex(e);
      Check(!ex.Patterned() && ex.ComponentCount() == 1 && ex.Regime() == REGIME_HILO, "legacy beam untouched");
      const ExcitationShares sh = { { 640.0, 1.0 } };
      Check(ex.At(1.0, 2.0, 0.7, sh) == ex.MeanAt(1.0, 2.0, 0.7, sh), "legacy beam: At = MeanAt");
      Check(ex.At(1.0, 2.0, 0.7, sh) == ex.F() * ex.Lateral(1.0, 2.0, 0.7) * ex.Depth(0.7, sh), "legacy beam: F x L x D");
   }
}

void FieldChecks()
{
   const double nS = 1.337, nI = 1.518, na = 1.4;
   int n = 0;
   for (int type : { SIM_2D, SIM_3D, SIM_LATTICE_2D, SIM_LATTICE_3D })
      for (int pol : { SIM_POL_S, SIM_POL_P, SIM_POL_CIRCULAR, SIM_POL_LINEAR_X, SIM_POL_UNPOLARIZED })
         for (double u : { 0.6, 0.9, 1.03 })
         {
            BeamSet set = MakeBeams(type, u, pol, 0.8);
            for (long long frame : { 0LL, 1LL, 7LL })
            {
               const PatternState st = PatternStateAt(set, frame, 0, 0.05);
               for (double nm : { 488.0, 640.0 })
               {
                  const Pattern pat = CompilePattern(st, nm, nS, nI, na, pol);
                  std::vector<double> env(st.beams.size(), 1.0);
                  for (int i = 0; i < 12; ++i)
                  {
                     const double x = 0.37 * i - 1.1, y = 0.21 * i + 0.4, z = 0.05 * i;
                     const double a = pat.At(x, y, z, env.data());
                     double b;
                     if (pol == SIM_POL_UNPOLARIZED)
                        b = 0.5 * ExplicitPattern(st, nm, nS, nI, na, pol, x, y, z, true, false) +
                            0.5 * ExplicitPattern(st, nm, nS, nI, na, pol, x, y, z, false, true);
                     else
                        b = ExplicitPattern(st, nm, nS, nI, na, pol, x, y, z);
                     Check(std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)),
                           "compiled terms = the explicit field sum (type " + SimTypeNames()[static_cast<size_t>(type)] +
                              ", " + SimPolarizationNames()[static_cast<size_t>(pol)] + ", u " + std::to_string(u) + ")");
                     ++n;
                  }
               }
            }
         }
   // A custom set: five beams with tilts, phases, amplitudes, an ellipticity, at mixed radii (one evanescent).
   {
      BeamSet set = MakeBeams(SIM_CUSTOM, 0, SIM_POL_S, 1.0);
      ApplyBeamRow(set, 0, { { 0, 0.0 }, { 1, 0.0 }, { 5, 0.7 }, { 6, 0.1 }, { 7, 30.0 }, { 8, 0.1 } });
      ApplyBeamRow(set, 1, { { 0, 0.5 }, { 1, 0.2 }, { 6, 0.25 }, { 7, 60.0 } });
      ApplyBeamRow(set, 2, { { 0, -0.5 }, { 1, 0.3 }, { 5, 1.3 }, { 7, 0.0 }, { 8, 0.25 } });
      ApplyBeamRow(set, 3, { { 0, 0.1 }, { 1, 1.02 }, { 7, 45.0 } });
      ApplyBeamRow(set, 4, { { 0, 0.3 }, { 1, -0.6 }, { 6, 0.9 }, { 7, 90.0 } });
      const PatternState st = PatternStateAt(set, 0, 0, 0.05);
      const Pattern pat = CompilePattern(st, 561.0, nS, nI, na, SIM_POL_S);
      std::vector<double> env(5, 1.0);
      for (int i = 0; i < 20; ++i)
      {
         const double x = 0.13 * i, y = -0.07 * i, z = 0.02 * i;
         const double a = pat.At(x, y, z, env.data()), b = ExplicitPattern(st, 561.0, nS, nI, na, SIM_POL_S, x, y, z);
         Check(std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)), "compiled terms = the explicit field sum (custom)");
      }
   }
   std::printf("fields: %d points\n", n);
}

void CycleChecks()
{
   const ExcitationShares sh = { { 640.0, 1.0 }, { 561.0, 0.0 } };
   for (int type : { SIM_2D, SIM_3D, SIM_LATTICE_2D, SIM_LATTICE_3D })
      for (int pol : { SIM_POL_S, SIM_POL_CIRCULAR })
      {
         const double u = type == SIM_2D ? 0.95 : 0.9;
         BeamSet set = MakeBeams(type, u, pol, 1.0);
         Excitation ex(SpecFor(set, ILLUM_GAUSSIAN));
         const int P = set.Period();
         Check(ex.Varies() && P == set.phases * set.angles, "period");
         for (int i = 0; i < 6; ++i)
         {
            const double x = 0.31 * i - 0.5, y = 0.17 * i + 0.2, z = 0.1 + 0.3 * i;
            double sum = 0.0;
            for (int f = 0; f < P; ++f)
            {
               ex.SetFrame(f);
               sum += ex.At(x, y, z, sh);
            }
            const double mean = ex.MeanAt(x, y, z, sh) * P;
            Check(std::fabs(sum - mean) <= 1e-10 * std::max(1.0, mean),
                  "cycle sum = nPhases x nAngles x the cycle mean (" + SimTypeNames()[static_cast<size_t>(type)] + ")");
         }
         // The plane mean of the frame's pattern, summed over the cycle, = the mean's plane mean.
         ex.SetFrame(0);
         double pm = 0.0;
         for (int f = 0; f < P; ++f)
         {
            ex.SetFrame(f);
            pm += ex.PlaneMean(0.3, -0.2, 0.4, 0.425, 0.05, sh);
         }
         Check(std::fabs(pm - P * ex.MeanPlaneMean(0.3, -0.2, 0.4, 0.425, sh)) <= 1e-9 * pm, "cycle of plane means");
         // The frame's plane mean = a fine quadrature of At over the cell and the plane.
         ex.SetFrame(3 % P);
         const double x0 = 0.3, y0 = -0.2, z0 = 0.4, z1 = 0.425, cell = 0.05;
         double q = 0.0;
         const int nq = 40;
         for (int a = 0; a < nq; ++a)
            for (int b = 0; b < nq; ++b)
               for (int c = 0; c < nq; ++c)
                  q += ex.At(x0 + (a + 0.5) / nq * cell - cell / 2, y0 + (b + 0.5) / nq * cell - cell / 2,
                             z0 + (c + 0.5) / nq * (z1 - z0), sh);
         q /= nq * nq * nq;
         const double pmean = ex.PlaneMean(x0, y0, z0, z1, cell, sh);
         Check(std::fabs(q - pmean) <= 2e-4 * std::max(1.0, q), "PlaneMean = the quadrature (" + std::to_string(q) +
                                                                    " vs " + std::to_string(pmean) + ")");
      }
}

void OrderChecks()
{
   for (int type : { SIM_2D, SIM_3D, SIM_LATTICE_2D, SIM_LATTICE_3D })
   {
      const BeamSet set = MakeBeams(type, 0.9, SIM_POL_S, 1.0);
      std::string why;
      Check(OrdersSeparate(set, why), "orders separate (" + SimTypeNames()[static_cast<size_t>(type)] + "): " + why);
   }
   // A set whose orders collide: the lattice's y pair stepping like its x pair.
   {
      BeamSet set = MakeBeams(SIM_LATTICE_3D, 0.9, SIM_POL_S, 1.0);
      set.beams[3].phaseStep = 2;
      set.beams[4].phaseStep = -2;
      std::string why;
      Check(!OrdersSeparate(set, why), "colliding orders are caught");
   }
   // The frame program.
   {
      BeamSet set = MakeBeams(SIM_3D, 0.9, SIM_POL_S, 1.0);
      Check(set.Period() == 15, "SIM3D period 15");
      const FrameLight a = FrameLightAt(set, 7), b = FrameLightAt(set, 15 + 7);
      Check(a.angle == 1 && a.phase == 2 && b.angle == 1 && b.phase == 2, "angle-major order, periodic");
      const PatternState s0 = PatternStateAt(set, 0, 0, 0.05), s1 = PatternStateAt(set, 5, 0, 0.05);
      Check(std::fabs(s1.rotDeg - 60.0) < 1e-12 && s0.rotDeg == 0.0, "the angle's rotation");
      set.order = SIM_ORDER_PHASE_MAJOR;
      const FrameLight c = FrameLightAt(set, 7);
      Check(c.angle == 1 && c.phase == 2, "phase-major order");
      // The matrix round-trips.
      std::vector<std::vector<double>> rows;
      std::string err;
      Check(ParseBeamMatrix(FormatBeamMatrix(set.beams), rows, err) && rows.size() == 3 &&
               rows[1].size() == static_cast<size_t>(kBeamColumns) && rows[1][0] == 0.9 && rows[1][9] == 1.0,
            "matrix round-trip: " + err);
      Check(!ParseBeamMatrix("0 0 x", rows, err), "matrix: a bad value is refused");
      // The phase drift: a walk per seed (the same path twice, another seed another path), the linear part.
      set.driftTurnsPerSqrtSec = 0.1;
      const double d1 = PhaseDriftTurns(set, 7, 1, 40, 0.05), d2 = PhaseDriftTurns(set, 7, 1, 40, 0.05);
      const double d3 = PhaseDriftTurns(set, 8, 1, 40, 0.05);
      Check(d1 == d2 && d1 != d3 && d1 != 0.0, "phase drift: a walk per seed");
      set.driftTurnsPerSqrtSec = 0;
      set.driftTurnsPerSec = 0.5;
      Check(std::fabs(PhaseDriftTurns(set, 7, 1, 40, 0.05) - 1.0 * 0.5 * 40 * 0.05) < 1e-12, "phase drift: linear");
   }
}

bool Photons(const ScopeSpec& spec, std::vector<std::vector<float>>& frames, std::string& err)
{
   frames.clear();
   ScopeMovieInfo info;
   return RenderScopePhotons(spec, [&](long, const std::vector<float>& ph) {
      frames.push_back(ph);
      return true;
   }, info, err);
}

void MovieChecks()
{
   ScopeSpec base;
   std::string err;
   Check(ParseScopeSpec("size=48 frames=15 x=6.58 y=-5.61 mt-mode=WideField npc-dye=None sim-type=SIM3D illum-bfp=0.6 exposure-ms=50 "
                        "psf-kernel-half-width-nm=1500",
                        base, err),
         "spec: " + err);
   std::vector<std::vector<float>> a, b;
   Check(Photons(base, a, err), "movie: " + err);
   ScopeSpec late = base;
   late["frame0"] = 5;
   late["frames"] = 10;
   late["start-sec"] = ScopeSpecGet(base, "start-sec") + 5 * 0.05;
   Check(Photons(late, b, err), "movie frame0: " + err);
   bool same = a.size() == 15 && b.size() == 10;
   // The same dyes at the same pattern state and clock: the frames agree closely (the bleach basis anchors differ:
   // within 1e-5 of the frame's peak).
   for (size_t f = 0; same && f < 10; ++f)
   {
      float peak = 0.0f;
      for (float v : a[f + 5])
         peak = std::max(peak, v);
      for (size_t i = 0; i < a[f + 5].size(); ++i)
         same = same && std::fabs(a[f + 5][i] - b[f][i]) <= 1e-5f * peak;
   }
   Check(same, "frame0: a movie from frame 5 = frames 5.. of the movie from 0");
   // Frames differ between phases and agree at the same state after a cycle (a fresh sample bleaches a little).
   float d01 = 0.0f, d0b = 0.0f, pk = 0.0f;
   for (size_t i = 0; i < a[0].size(); ++i)
   {
      d01 = std::max(d01, std::fabs(a[0][i] - a[1][i]));
      pk = std::max(pk, a[0][i]);
   }
   Check(d01 > 0.05f * pk, "the phases differ");
   (void)d0b;
   // Mean field vs per dye: the fringe's amplitude in the same frame.
   ScopeSpec mf = base, pd = base;
   mf["frames"] = 1;
   pd["frames"] = 1;
   mf["mean-field-density-per-um2"] = 0;
   mf["mean-field-max-emitters"] = 0;
   pd["mean-field-density-per-um2"] = 1e9;
   pd["mean-field-max-emitters"] = 1e9;
   std::vector<std::vector<float>> fm, fp;
   Check(Photons(mf, fm, err) && Photons(pd, fp, err), "mean field / per dye: " + err);
   if (fm.size() == 1 && fp.size() == 1)
   {
      double sm = 0, sp = 0, dm = 0;
      for (size_t i = 0; i < fm[0].size(); ++i)
      {
         sm += fm[0][i];
         sp += fp[0][i];
      }
      for (size_t i = 0; i < fm[0].size(); ++i)
         dm = std::max(dm, std::fabs(fm[0][i] * sp / sm - fp[0][i]));
      const double rel = std::fabs(sm - sp) / sp;
      Check(rel < 0.03, "mean field = per dye in total photons (" + std::to_string(rel) + ")");
      std::printf("mean field vs per dye: totals %.4g / %.4g, largest pixel difference %.3g of the mean %.3g\n", sm, sp,
                  dm, sp / fp[0].size());
   }
   // sim-type Off: the legacy beam (no beam set), its frames exactly those without the pattern options.
   ScopeSpec off = base, plain;
   off["sim-type"] = 0;
   off["frames"] = 1;
   Check(ParseScopeSpec("size=48 frames=1 x=6.58 y=-5.61 mt-mode=WideField npc-dye=None illum-bfp=0.6 exposure-ms=50 "
                        "psf-kernel-half-width-nm=1500",
                        plain, err),
         "spec: " + err);
   std::vector<std::vector<float>> fo, fq;
   Check(Photons(off, fo, err) && Photons(plain, fq, err) && fo.size() == 1 && fo[0] == fq[0],
         "sim-type Off = the legacy beam bit for bit");
   Check(ScopeBeamSet(off).beams.empty() && ScopeBeamSet(base).beams.size() == 3, "ScopeBeamSet");
}

} // namespace

int main()
{
   const auto t0 = std::chrono::steady_clock::now();
   FringeChecks();
   FieldChecks();
   CycleChecks();
   OrderChecks();
   MovieChecks();
   const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   if (g_fail)
   {
      std::printf("%d check(s) failed (%.1f s)\n", g_fail, sec);
      return 1;
   }
   std::printf("sim_pattern: all passed (%.1f s)\n", sec);
   return 0;
}
