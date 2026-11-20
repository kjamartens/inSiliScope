///////////////////////////////////////////////////////////////////////////////
// FILE:          BfpField.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See BfpField.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BfpField.h"
#include "BfpMaps.h"
#include "ChirpZ.h"
#include "Parallel.h"
#include "SimPattern.h"
#include "Timing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <mutex>

namespace sim {

namespace {
using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr int kAnalysisPx = 256;      // the analysis raster over the pupil square
constexpr int kMaxSide = 512;         // lateral samples per side of a volume
constexpr int kMaxPupil = 2048;       // pupil samples per axis
constexpr int kMaxPlanes = 256;       // per part
constexpr int kIncoherentPupil = 128; // pupil samples per axis when no light is coherent (each point its own plane wave)
constexpr double kLitRel = 1e-8;      // a pupil point counts above this x the peak (amplitude 1e-4)
constexpr double kSigRel = 1e-4;      // ... and for the geometry (reach, spans, regions)
// The pupil grid's period keeps all but this share of the propagating light's power out of the window (the steepest
// rays of a focused beam's edge, a Gaussian ring's tail towards u = 1, reach 200 um at the volume's top: a period over
// them took a full-pupil spot to a 1227^2 pupil, minutes; their light, wrapped round, lands spread thin in the window).
constexpr double kReachPower = 1e-3;
constexpr int kTanBins = 1000;        // the steepness histogram for it: tan 0..50 in steps of 0.05
constexpr double kZSnap = 0.005;      // planes on the light history's depth step (LightHistory.h kLightDepthStepUm)
constexpr double kMaxDxUm = 1.0;
constexpr int kIncoherentBins = 256;
constexpr double kUEdge = 1e-3;       // |u| kept this far from 1 (BeamFieldOf's kUGap)

double Snap(double z) { return std::round(z / kZSnap) * kZSnap; }

void CubicWeights(double t, double w[4])
{
   const double t2 = t * t, t3 = t2 * t;
   w[0] = -0.5 * t3 + t2 - 0.5 * t;
   w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
   w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
   w[3] = 0.5 * t3 - 0.5 * t2;
}

// Bicubic (Catmull-Rom) on an n x m grid of floats (origin x0, y0, step dx), 0 outside.
double Bicubic(const float* g, int nx, int ny, double x0, double y0, double dx, double x, double y)
{
   const double fx = (x - x0) / dx, fy = (y - y0) / dx;
   if (!(fx > -1.0 && fx < nx && fy > -1.0 && fy < ny))
      return 0.0;
   const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
   double wx[4], wy[4];
   CubicWeights(fx - i, wx);
   CubicWeights(fy - j, wy);
   double s = 0.0;
   for (int b = 0; b < 4; ++b)
   {
      const int jj = j - 1 + b;
      if (jj < 0 || jj >= ny)
         continue;
      const float* row = g + static_cast<size_t>(jj) * nx;
      double r = 0.0;
      for (int a = 0; a < 4; ++a)
      {
         const int ii = i - 1 + a;
         if (ii >= 0 && ii < nx)
            r += wx[a] * row[ii];
      }
      s += wy[b] * r;
   }
   return s;
}

// The legacy beam's envelope (Excitation::EnvelopeAt), peak 1.
struct Env
{
   int profile = 1;
   double sig = 1, r0 = 1, edge = 1, flatTop0 = 1, integral = 1, scale = 1;
   void Make(int prof, double fwhm)
   {
      profile = prof;
      fwhm = std::max(1e-6, fwhm);
      sig = fwhm / 2.3548;
      r0 = fwhm / 2.0;
      edge = std::max(1e-6, 0.15 * r0);
      flatTop0 = 1.0 / (1.0 + std::exp(-r0 / edge));
      // Its power: Gaussian 2 pi sigma^2; FlatTop (and Flat, which has no finite power: the FlatTop's) numerically.
      if (profile == 1)
         integral = 2.0 * kPi * sig * sig;
      else
      {
         const int n = 4000;
         const double top = r0 + 40.0 * edge, dr = top / n;
         double s = 0.0;
         for (int k = 0; k < n; ++k)
         {
            const double r = (k + 0.5) * dr;
            s += 2.0 * kPi * r / (1.0 + std::exp((r - r0) / edge)) / flatTop0 * dr;
         }
         integral = s;
      }
      scale = profile == 1 ? sig : (profile == 2 ? edge : std::numeric_limits<double>::infinity());
   }
   double At(double r2) const
   {
      if (profile == 0)
         return 1.0;
      if (profile == 1)
         return std::exp(-r2 / (2.0 * sig * sig));
      return 1.0 / (1.0 + std::exp((std::sqrt(r2) - r0) / edge)) / flatTop0;
   }
   double Reach(double frac) const
   {
      if (profile == 0)
         return std::numeric_limits<double>::infinity();
      if (profile == 1)
         return sig * std::sqrt(-2.0 * std::log(frac));
      return r0 + edge * std::log(1.0 / (frac * flatTop0) - 1.0);
   }
};

// One 1D transform out[p] = sum_m in[m] exp(i k_m x_p), by chirp-Z or (short inputs) a table.
struct Dft1
{
   int M = 0, P = 0;
   bool direct = true;
   CztPlan plan;
   std::vector<cd> table;   // M x P
   void Init(int M_, double k0, double dk, int P_, double x0, double dx)
   {
      M = M_;
      P = P_;
      direct = M < 32;
      if (direct)
      {
         table.resize(static_cast<size_t>(M) * P);
         for (int m = 0; m < M; ++m)
            for (int p = 0; p < P; ++p)
               table[static_cast<size_t>(m) * P + p] = std::polar(1.0, (k0 + m * dk) * (x0 + p * dx));
      }
      else
         plan = CztPlan(M, dk, k0, P, dx, x0);
   }
   struct Scratch
   {
      std::vector<double> inRe, inIm, outRe, outIm, aRe, aIm;
   };
   void Apply(const cd* in, cd* out, Scratch& s) const
   {
      if (direct)
      {
         std::fill(out, out + P, cd(0.0, 0.0));
         for (int m = 0; m < M; ++m)
         {
            const cd a = in[m];
            if (a.real() == 0.0 && a.imag() == 0.0)
               continue;
            const cd* t = table.data() + static_cast<size_t>(m) * P;
            for (int p = 0; p < P; ++p)
               out[p] += a * t[p];
         }
         return;
      }
      s.inRe.resize(static_cast<size_t>(M));
      s.inIm.resize(static_cast<size_t>(M));
      s.outRe.resize(static_cast<size_t>(P));
      s.outIm.resize(static_cast<size_t>(P));
      s.aRe.resize(plan.ScratchPerBatch(1));
      s.aIm.resize(plan.ScratchPerBatch(1));
      for (int m = 0; m < M; ++m)
      {
         s.inRe[static_cast<size_t>(m)] = in[m].real();
         s.inIm[static_cast<size_t>(m)] = in[m].imag();
      }
      const double* ir[1] = { s.inRe.data() };
      const double* ii[1] = { s.inIm.data() };
      double* orr[1] = { s.outRe.data() };
      double* oi[1] = { s.outIm.data() };
      plan.Apply<1>(ir, ii, orr, oi, s.aRe.data(), s.aIm.data());
      for (int p = 0; p < P; ++p)
         out[p] = cd(s.outRe[static_cast<size_t>(p)], s.outIm[static_cast<size_t>(p)]);
   }
};

struct Evaluator : BfpProgram::ImageSampler
{
   const BfpProgram* prog = nullptr;
   std::vector<std::shared_ptr<const BfpImage>> images;
   double umax = 1;
   double vars[kBfpVars] = {};
   double Sample(int i, double ux, double uy, double t) const override
   {
      return i >= 0 && i < static_cast<int>(images.size()) && images[static_cast<size_t>(i)]
                ? images[static_cast<size_t>(i)]->Sample(ux, uy, t, umax)
                : 0.0;
   }
   void Eval(double ux, double uy, double t, double* maps, std::vector<double>& scratch) const
   {
      double v[kBfpVars];
      std::memcpy(v, vars, sizeof(v));
      v[BV_UX] = ux;
      v[BV_UY] = uy;
      v[BV_R] = std::hypot(ux, uy);
      v[BV_PHI] = (ux == 0.0 && uy == 0.0) ? 0.0 : std::atan2(uy, ux);
      v[BV_T] = t;
      prog->Run(v, maps, scratch, this);
   }
};

bool LitAt(double t, double T, double duty)
{
   if (!(duty < 1.0))
      return true;
   if (!(duty > 0.0))
      return false;
   double d = std::fmod(t - T, 1.0);
   if (d < 0)
      d += 1.0;
   return d < duty;
}

// A pupil point's values (the maps that the field needs), on the field grid.
struct Pt
{
   float I = 0, phase = 0, pol = 0, polLab = 0, ellip = 0, tx = 0, ty = 0, focus = 0, T = 0, duty = 1;
   uint8_t coh = 1;
};

uint64_t Mix(uint64_t h, double v) { return BfpHashBytes(&v, sizeof(v), h); }
uint64_t Mix(uint64_t h, const std::string& s) { return BfpHashBytes(s.data(), s.size(), Mix(h, static_cast<double>(s.size()))); }

enum Family { FAM_REL = 0, FAM_EVA = 1, FAM_ABS = 2 };
} // namespace

double BfpBicubic(const float* g, int nx, int ny, double x0, double y0, double dx, double x, double y)
{
   return Bicubic(g, nx, ny, x0, y0, dx, x, y);
}

double BfpVolume::Plane(int k, double x, double y) const
{
   return Bicubic(v.data() + static_cast<size_t>(k) * nx * ny, nx, ny, x0, y0, dx, x, y);
}

double BfpVolume::At(double x, double y, double zAbs, double pivot) const
{
   const int nz = Nz();
   if (nz == 0)
      return 0.0;
   const double zeta = zAbs - (relative ? pivot : 0.0);
   double val;
   if (!(zeta > z[0]))
   {
      if (!extendBottom && zeta < z[0] - 1e-9)
         return 0.0;
      const double d = zeta - z[0];
      val = Plane(0, x - d * sx, y - d * sy);
   }
   else if (!(zeta < z[static_cast<size_t>(nz - 1)]))
   {
      const double d = zeta - z[static_cast<size_t>(nz - 1)];
      if (!extendTop && d > 1e-9)
         return 0.0;
      val = Plane(nz - 1, x - d * sx, y - d * sy);
   }
   else
   {
      const size_t k = static_cast<size_t>(std::upper_bound(z.begin(), z.end(), zeta) - z.begin()) - 1;
      const double za = z[k], zb = z[k + 1], a = (zeta - za) / (zb - za);
      const double v0 = Plane(static_cast<int>(k), x - (zeta - za) * sx, y - (zeta - za) * sy);
      const double v1 = Plane(static_cast<int>(k + 1), x - (zeta - zb) * sx, y - (zeta - zb) * sy);
      val = (1.0 - a) * v0 + a * v1;
   }
   return val > 0.0 ? val : 0.0;
}

double BfpLightField::At(double x, double y, double z, double pivot) const
{
   double s = 0.0;
   for (const BfpVolume& p : parts)
      s += p.At(x, y, z, pivot);
   return s;
}

int BfpLight::LineOf(double nm) const
{
   int best = 0;
   double d = std::numeric_limits<double>::infinity();
   for (size_t l = 0; l < lines_.size(); ++l)
      if (std::fabs(lines_[l].nm - nm) < d)
      {
         d = std::fabs(lines_[l].nm - nm);
         best = static_cast<int>(l);
      }
   return best;
}

const BfpLightField& BfpLight::State(int l, int state) const
{
   const Line& L = lines_[static_cast<size_t>(l)];
   if (L.states.empty())
      return L.mean;
   const int n = static_cast<int>(L.states.size());
   return L.states[static_cast<size_t>(((state % n) + n) % n)];
}

std::shared_ptr<const BfpLight> BfpLight::Slim() const
{
   auto s = std::make_shared<BfpLight>(*this);   // (the volumes copied, then dropped: a copy of the small parts)
   for (Line& L : s->lines_)
   {
      L.states.clear();
      L.mean.parts.clear();
   }
   s->slim_ = true;
   return s;
}

size_t BfpLight::Bytes() const
{
   size_t b = 0;
   for (const Line& L : lines_)
   {
      for (const BfpLightField& f : L.states)
         for (const BfpVolume& v : f.parts)
            b += v.Bytes();
      for (const BfpVolume& v : L.mean.parts)
         b += v.Bytes();
   }
   return b;
}

std::string BfpTypeEquations(int simType)
{
   switch (simType)
   {
   case 6:   // LightSheet: an elliptical Gaussian spot, thin radially (the sheet's thickness across the beam), long
             // along y (its width); unpolarized.
      return "# Inclined light sheet: an elliptical spot at u = bfp\n"
             "cs = sqrt(max(1 - bfp^2; 0.01))\n"
             "a = nm*1e-3 / (pi*ns*(sheetT/cs/1.1774))\n"
             "b = nm*1e-3 / (pi*ns*(sheetW/1.1774))\n"
             "I = exp(-2*(((ux - bfp)/a)^2 + (uy/b)^2))\n"
             "coherent = 1\n"
             "pol = unpolarized";
   case 7:   // RingTIRF: the legacy spot at u = bfp, scanned once round within every exposure.
      return "# Ring TIRF: the spot at u = bfp, turned once round per exposure\n"
             "scanRot = 360*t\n"
             "I = exp(-2*((ux - bfp)^2 + uy^2)/w^2)\n"
             "coherent = 1\n"
             "pol = unpolarized";
   default:   // Maps: the legacy beam as a map.
      return "# The legacy beam: a Gaussian spot at u = bfp\n"
             "I = exp(-2*((ux - bfp)^2 + uy^2)/w^2)\n"
             "coherent = 1\n"
             "pol = unpolarized";
   }
}

int BfpTypeSlots(int simType) { return simType == 7 ? 64 : 0; }

// ---------------------------------------------------------------------------------------------------------------------

struct BfpBuilder
{
   const BfpLightParams& P;
   BfpLight& out;
   BfpProgram prog;
   Evaluator ev;
   std::vector<int> pages;
   bool tdep = false, scans = false, timing = false, needRegions = false;
   int S = 1, C = 0, period = 1;
   double umax = 1;
   Env env;
   double Wh = 10;
   std::string err;

   BfpBuilder(const BfpLightParams& p, BfpLight& o) : P(p), out(o) {}

   double SlotT(int s) const { return (s + 0.5) / S; }
   int StateOf(int s) const { return C >= 1 && S > 1 ? static_cast<int>(static_cast<long long>(s) * C / S) : 0; }

   void SetVars(double nm)
   {
      const double K = 2.0 * kPi * P.nS / (nm * 1e-3);
      const double ws = env.integral > 0 ? std::max(1e-6, P.envFwhmUm) / 1.1774 : 1.0;
      double* v = ev.vars;
      v[BV_NM] = nm;
      v[BV_UMAX] = umax;
      v[BV_UC] = 1.0;
      v[BV_PI] = kPi;
      v[BV_BFP] = P.bfp;
      v[BV_W] = 2.0 / (K * ws);
      v[BV_FWHM] = P.envFwhmUm;
      v[BV_NS] = P.nS;
      v[BV_NI] = P.nI;
      v[BV_NA] = P.na;
      v[BV_F] = P.fObjUm;
      v[BV_SHEET_T] = P.sheetTUm;
      v[BV_SHEET_W] = P.sheetWUm > 0 ? P.sheetWUm : P.envFwhmUm;
      v[BV_UNPOLARIZED] = kBfpUnpolarized;
   }

   // The slot's scan: rotation (rad) and shift (um).
   void ScanOf(int s, double& rot, double& dxs, double& dys, std::vector<double>& scratch) const
   {
      double m[kBfpMaps];
      ev.Eval(0.0, 0.0, SlotT(s), m, scratch);
      rot = m[BM_SCAN_ROT] * kDeg;
      dxs = P.fObjUm * std::tan(m[BM_SCAN_X] * kDeg);
      dys = P.fObjUm * std::tan(m[BM_SCAN_Y] * kDeg);
   }

   Pt PointAt(double ux, double uy, double t, std::vector<double>& scratch) const
   {
      double m[kBfpMaps];
      ev.Eval(ux, uy, t, m, scratch);
      Pt p;
      const double r = std::hypot(ux, uy);
      p.I = (r > umax || !(m[BM_I] > 0.0) || !std::isfinite(m[BM_I])) ? 0.0f : static_cast<float>(m[BM_I]);
      p.phase = static_cast<float>(m[BM_PHASE]);
      p.pol = static_cast<float>(m[BM_POL]);
      p.polLab = static_cast<float>(m[BM_POL_LAB]);
      p.ellip = static_cast<float>(m[BM_ELLIP]);
      p.tx = static_cast<float>(m[BM_TILT_X]);
      p.ty = static_cast<float>(m[BM_TILT_Y]);
      p.focus = static_cast<float>(m[BM_FOCUS]);
      p.T = static_cast<float>(m[BM_T]);
      p.duty = static_cast<float>(m[BM_DUTY]);
      p.coh = m[BM_COHERENT] != 0.0 ? 1 : 0;
      return p;
   }

   bool Setup()
   {
      if (!prog.Compile(P.text, err))
         return false;
      ev.prog = &prog;
      umax = P.na / P.nS;
      ev.umax = umax;
      for (const std::string& name : prog.Images())
      {
         std::string e;
         auto im = BfpFindImage(name, e);
         if (!im)
         {
            err = e;
            return false;
         }
         ev.images.push_back(im);
         pages.push_back(im->pages);
      }
      for (int m : { BM_SCAN_X, BM_SCAN_Y, BM_SCAN_ROT })
         if (prog.DependsOn(m, pages) & BD_U)
         {
            err = BfpMapNames()[static_cast<size_t>(m)] + " may depend on t only (not on the pupil position)";
            return false;
         }
      for (int m = 0; m < kBfpMaps; ++m)
      {
         if (m == BM_SCAN_X || m == BM_SCAN_Y || m == BM_SCAN_ROT)
            continue;
         if (prog.DependsOn(m, pages) & BD_T)
            tdep = true;
      }
      scans = prog.Sets(BM_SCAN_X) || prog.Sets(BM_SCAN_Y) || prog.Sets(BM_SCAN_ROT);
      timing = prog.Sets(BM_T) || prog.Sets(BM_DUTY);
      const bool anyT = tdep || timing ||
                        ((prog.DependsOn(BM_SCAN_X, pages) | prog.DependsOn(BM_SCAN_Y, pages) |
                          prog.DependsOn(BM_SCAN_ROT, pages)) & BD_T);
      S = P.slots > 0 ? std::min(P.slots, 4096) : (anyT ? 64 : 1);
      C = std::max(0, P.cycleFrames);
      period = (C >= 1 && S > 1) ? std::min(C, S) : 1;
      if (period > 1)
         C = period;
      needRegions = prog.Sets(BM_FOCUS) || !P.anchorFocus;
      env.Make(P.profile, P.envFwhmUm);
      const double fov = std::max(1.0, P.fovSpanUm);
      const double reach = env.Reach(1e-3);
      Wh = std::max(fov / 2.0 + 3.0, std::min(1.5 * fov, std::isfinite(reach) ? reach : 1.5 * fov));
      return true;
   }

   // ----- per line -----
   struct Analysis
   {
      double Imax = 0;
      double ux0 = 1e9, ux1 = -1e9, uy0 = 1e9, uy1 = -1e9;   // lit (kLitRel) bounding box
      bool prop = false, evan = false, incoh = false, coh = false, mixed = false;
      int propRegions = 0;
      double tanMax = 0, shiftMax = 0, span = 0, dg = 0, sigMin = 1e9, sbx = 0, sby = 0, wsum = 0;
      double dMin = 1e9, dMax = 0, devMax = 0, uOuter = 0;
      std::vector<double> tanW = std::vector<double>(kTanBins + 1, 0.0);   // propagating intensity by tan(angle)
   };

   // The analysis raster's points for one evaluation (a slot's t), with timing at t.
   void AnalyseSlot(double nm, double t, std::vector<Pt>& pts, Analysis& A, bool firstPass, std::vector<double>& scratch)
   {
      const int n = kAnalysisPx;
      const double du = 2.0 * umax / n;
      pts.resize(static_cast<size_t>(n) * n);
      ParallelFor(static_cast<unsigned>(n), [&](unsigned j) {
         std::vector<double> sc;
         for (int i = 0; i < n; ++i)
         {
            const double ux = -umax + (i + 0.5) * du, uy = -umax + (j + 0.5) * du;
            Pt p = PointAt(ux, uy, t, sc);
            if (!LitAt(t, p.T, p.duty))
               p.I = 0;
            pts[static_cast<size_t>(j) * n + i] = p;
         }
      });
      (void)scratch;
      if (firstPass)
      {
         for (const Pt& p : pts)
            A.Imax = std::max(A.Imax, static_cast<double>(p.I));
         return;
      }
      if (!(A.Imax > 0))
         return;
      const double K = 2.0 * kPi * P.nS / (nm * 1e-3);
      const double thrL = kLitRel * A.Imax, thrS = kSigRel * A.Imax;
      double cx0 = 1e9, cx1 = -1e9, cy0 = 1e9, cy1 = -1e9, g0 = 1e18, g1 = -1e18;
      bool cohProp = false, cohEvan = false;
      for (int j = 0; j < n; ++j)
         for (int i = 0; i < n; ++i)
         {
            const Pt& p = pts[static_cast<size_t>(j) * n + i];
            if (!(p.I > thrL))
               continue;
            const double ux = -umax + (i + 0.5) * du, uy = -umax + (j + 0.5) * du;
            A.ux0 = std::min(A.ux0, ux);
            A.ux1 = std::max(A.ux1, ux);
            A.uy0 = std::min(A.uy0, uy);
            A.uy1 = std::max(A.uy1, uy);
            if (!(p.I > thrS))
               continue;
            double r = std::hypot(ux, uy);
            if (p.I > 1e-3 * A.Imax)
               A.uOuter = std::max(A.uOuter, r);
            if (std::fabs(r - 1.0) < kUEdge)
               r = r < 1.0 ? 1.0 - kUEdge : 1.0 + kUEdge;
            const bool evn = r > 1.0;
            const double tilt = std::hypot(p.tx, p.ty) + std::fabs(p.focus);
            A.shiftMax = std::max(A.shiftMax, P.fObjUm * std::tan(std::min(89.0, tilt) * kDeg));
            if (p.coh)
            {
               A.coh = true;
               cx0 = std::min(cx0, ux);
               cx1 = std::max(cx1, ux);
               cy0 = std::min(cy0, uy);
               cy1 = std::max(cy1, uy);
               (evn ? cohEvan : cohProp) = true;
            }
            else
               A.incoh = true;
            if (evn)
            {
               A.evan = true;
               const double d = nm * 1e-3 / (4.0 * kPi * P.nS * std::sqrt(r * r - 1.0));
               A.dMin = std::min(A.dMin, d);
               A.dMax = std::max(A.dMax, d);
            }
            else
            {
               A.prop = true;
               const double kz = std::sqrt(1.0 - r * r), tn = r / kz;
               A.tanMax = std::max(A.tanMax, tn);
               A.tanW[static_cast<size_t>(std::min(static_cast<double>(kTanBins), tn / 50.0 * kTanBins))] += p.I;
               const double w = p.I;
               const double cphi = r > 0 ? ux / r : 1.0, sphi = r > 0 ? uy / r : 0.0;
               A.sbx += w * tn * cphi;
               A.sby += w * tn * sphi;
               A.wsum += w;
            }
         }
      if (cohProp && cohEvan)
         A.mixed = true;
      if (cx1 >= cx0)
         A.span = std::max(A.span, std::max(cx1 - cx0, cy1 - cy0));
      (void)g0;
      (void)g1;
      (void)K;
   }

   // The second pass over a slot's raster: the axial beats (needs the mean direction), the regions' widths.
   void AnalyseSlot2(double nm, const std::vector<Pt>& pts, Analysis& A)
   {
      if (!(A.Imax > 0))
         return;
      const int n = kAnalysisPx;
      const double du = 2.0 * umax / n;
      const double K = 2.0 * kPi * P.nS / (nm * 1e-3);
      const double thrS = kSigRel * A.Imax;
      const double sbx = A.wsum > 0 ? A.sbx / A.wsum : 0.0, sby = A.wsum > 0 ? A.sby / A.wsum : 0.0;
      double g0 = 1e18, g1 = -1e18;
      std::vector<int> lab(pts.size(), -1);
      int regions = 0, propRegions = 0;
      for (int j = 0; j < n; ++j)
         for (int i = 0; i < n; ++i)
         {
            const size_t q = static_cast<size_t>(j) * n + i;
            const Pt& p = pts[q];
            if (!(p.I > thrS))
               continue;
            const double ux = -umax + (i + 0.5) * du, uy = -umax + (j + 0.5) * du;
            double r = std::hypot(ux, uy);
            if (std::fabs(r - 1.0) < kUEdge)
               r = r < 1.0 ? 1.0 - kUEdge : 1.0 + kUEdge;
            if (r < 1.0)
            {
               const double kz = std::sqrt(1.0 - r * r), tn = r / kz;
               const double cphi = r > 0 ? ux / r : 1.0, sphi = r > 0 ? uy / r : 0.0;
               if (p.coh)
               {
                  const double g = K * (kz + sbx * ux + sby * uy);
                  g0 = std::min(g0, g);
                  g1 = std::max(g1, g);
               }
               else
                  A.devMax = std::max(A.devMax, std::hypot(tn * cphi - sbx, tn * sphi - sby));
            }
            if (lab[q] >= 0)
               continue;
            // A region: 4-connected lit points; its intensity-weighted rms radius in u.
            std::vector<size_t> stack{ q };
            lab[q] = regions;
            double w = 0, mx = 0, my = 0, mxx = 0, myy = 0;
            bool anyProp = false;
            while (!stack.empty())
            {
               const size_t c = stack.back();
               stack.pop_back();
               const int ci = static_cast<int>(c % n), cj = static_cast<int>(c / n);
               const double cux = -umax + (ci + 0.5) * du, cuy = -umax + (cj + 0.5) * du;
               const double I = pts[c].I;
               w += I;
               mx += I * cux;
               my += I * cuy;
               mxx += I * cux * cux;
               myy += I * cuy * cuy;
               anyProp = anyProp || (pts[c].coh && std::hypot(cux, cuy) < 1.0);
               const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
               for (const auto& d : nb)
               {
                  const int a = ci + d[0], b = cj + d[1];
                  if (a < 0 || b < 0 || a >= n || b >= n)
                     continue;
                  const size_t qq = static_cast<size_t>(b) * n + a;
                  if (lab[qq] < 0 && pts[qq].I > thrS)
                  {
                     lab[qq] = regions;
                     stack.push_back(qq);
                  }
               }
            }
            regions++;
            if (anyProp)
               propRegions++;
            if (w > 0)
            {
               mx /= w;
               my /= w;
               const double var = std::max(0.0, mxx / w - mx * mx) + std::max(0.0, myy / w - my * my);
               A.sigMin = std::min(A.sigMin, std::max(du / 2.0, std::sqrt(var / 2.0)));
            }
         }
      if (g1 >= g0)
         A.dg = std::max(A.dg, g1 - g0);
      A.propRegions = std::max(A.propRegions, propRegions);
   }

   // ----- the field of one set of lit points -----
   struct Grid
   {
      int M = 0;                 // pupil samples per axis, centred
      double du = 0;
      int mx0 = 0, mx1 = -1, my0 = 0, my1 = -1;   // the evaluated box
      double U(int m) const { return (m - (M - 1) / 2) * du; }
   };
   struct PlaneSet
   {
      int fam = FAM_REL;
      std::vector<double> z;   // relative (REL) or absolute
      bool extendTop = true, extendBottom = true;
   };

   // Intensity planes of one field on a centred window of half width Wf (nxf samples), per plane set (family).
   struct FieldPlanes
   {
      int nx = 0;
      double x0 = 0;
      std::vector<std::vector<float>> fam;   // per plane set: nz x nx x nx
      std::vector<double> sx, sy;            // the field's mean direction per plane set
      std::vector<double> power;             // per plane set: the raw pupil power (sum I du^2)
      double powerEva = 0, powerAll = 0;     // the raw pupil power evanescent and in all (the regime's share)
   };

   void ComputeField(double nm, const Grid& G, const std::vector<Pt>& pts, const std::vector<uint8_t>& mask, double Imax,
                     const std::vector<PlaneSet>& sets, int nxf, double dx, double pivot, FieldPlanes& F)
   {
      const double K = 2.0 * kPi * P.nS / (nm * 1e-3);
      const int bw = G.mx1 - G.mx0 + 1, bh = G.my1 - G.my0 + 1;
      const double xf0 = -(nxf - 1) / 2 * dx;
      F.nx = nxf;
      F.x0 = xf0;
      F.fam.assign(sets.size(), std::vector<float>());
      F.sx.assign(sets.size(), 0.0);
      F.sy.assign(sets.size(), 0.0);
      F.power.assign(sets.size(), 0.0);
      const double thrL = kLitRel * Imax;
      auto lit = [&](size_t q) { return mask[q] && pts[q].I > thrL; };
      auto UX = [&](int i) { return G.U(G.mx0 + i); };
      auto UY = [&](int j) { return G.U(G.my0 + j); };
      const size_t np = static_cast<size_t>(bw) * bh;

      // Regions (for the focus' centre and the anchor's phase).
      std::vector<int> lab(np, -1);
      std::vector<double> cxR, cyR;
      std::vector<int> ciR, cjR;
      if (needRegions)
      {
         for (size_t q = 0; q < np; ++q)
         {
            if (lab[q] >= 0 || !lit(q))
               continue;
            const int R = static_cast<int>(cxR.size());
            std::vector<size_t> st{ q };
            lab[q] = R;
            double w = 0, mx = 0, my = 0;
            while (!st.empty())
            {
               const size_t c = st.back();
               st.pop_back();
               const int ci = static_cast<int>(c % bw), cj = static_cast<int>(c / bw);
               w += pts[c].I;
               mx += pts[c].I * UX(ci);
               my += pts[c].I * UY(cj);
               const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
               for (const auto& d : nb)
               {
                  const int a = ci + d[0], b = cj + d[1];
                  if (a < 0 || b < 0 || a >= bw || b >= bh)
                     continue;
                  const size_t qq = static_cast<size_t>(b) * bw + a;
                  if (lab[qq] < 0 && lit(qq))
                  {
                     lab[qq] = R;
                     st.push_back(qq);
                  }
               }
            }
            cxR.push_back(w > 0 ? mx / w : 0.0);
            cyR.push_back(w > 0 ? my / w : 0.0);
            ciR.push_back(std::min(bw - 1, std::max(0, static_cast<int>(std::lround(cxR.back() / G.du)) + (G.M - 1) / 2 - G.mx0)));
            cjR.push_back(std::min(bh - 1, std::max(0, static_cast<int>(std::lround(cyR.back() / G.du)) + (G.M - 1) / 2 - G.my0)));
         }
      }
      // The landing point s(u) of a point for region R (um).
      auto shiftOf = [&](int i, int j, int R, double& sx, double& sy) {
         const Pt& p = pts[static_cast<size_t>(j) * bw + i];
         double ax = p.tx, ay = p.ty;
         if (p.focus != 0.0f && R >= 0)
         {
            const double dxu = cxR[static_cast<size_t>(R)] - UX(i), dyu = cyR[static_cast<size_t>(R)] - UY(j);
            const double d = std::hypot(dxu, dyu);
            if (d > 0)
            {
               ax += p.focus * dxu / d;
               ay += p.focus * dyu / d;
            }
         }
         sx = P.fObjUm * std::tan(std::max(-89.0, std::min(89.0, ax)) * kDeg);
         sy = P.fObjUm * std::tan(std::max(-89.0, std::min(89.0, ay)) * kDeg);
      };
      // The phase potential Phi(u) = integral of s . du from the region's centre (along x on its row, then along y).
      std::vector<double> phi(np, 0.0);
      {
         const int nR = needRegions ? static_cast<int>(cxR.size()) : 1;
         for (int R = 0; R < nR; ++R)
         {
            int i0 = bw, i1 = -1, j0 = bh, j1 = -1;
            if (needRegions)
            {
               for (size_t q = 0; q < np; ++q)
                  if (lab[q] == R)
                  {
                     const int i = static_cast<int>(q % bw), j = static_cast<int>(q / bw);
                     i0 = std::min(i0, i);
                     i1 = std::max(i1, i);
                     j0 = std::min(j0, j);
                     j1 = std::max(j1, j);
                  }
            }
            else
            {
               i0 = 0;
               i1 = bw - 1;
               j0 = 0;
               j1 = bh - 1;
            }
            if (i1 < i0)
               continue;
            const int ci = needRegions ? std::min(i1, std::max(i0, ciR[static_cast<size_t>(R)])) : bw / 2;
            const int cj = needRegions ? std::min(j1, std::max(j0, cjR[static_cast<size_t>(R)])) : bh / 2;
            const int W = i1 - i0 + 1, H = j1 - j0 + 1;
            std::vector<double> sxv(static_cast<size_t>(W) * H), syv(sxv.size()), pot(sxv.size(), 0.0);
            bool any = false;
            for (int j = j0; j <= j1; ++j)
               for (int i = i0; i <= i1; ++i)
               {
                  double a, b;
                  shiftOf(i, j, needRegions ? R : -1, a, b);
                  sxv[static_cast<size_t>(j - j0) * W + (i - i0)] = a;
                  syv[static_cast<size_t>(j - j0) * W + (i - i0)] = b;
                  any = any || a != 0.0 || b != 0.0;
               }
            if (!any)
               continue;
            auto at = [&](int i, int j) { return static_cast<size_t>(j - j0) * W + (i - i0); };
            for (int i = ci + 1; i <= i1; ++i)
               pot[at(i, cj)] = pot[at(i - 1, cj)] + G.du * 0.5 * (sxv[at(i - 1, cj)] + sxv[at(i, cj)]);
            for (int i = ci - 1; i >= i0; --i)
               pot[at(i, cj)] = pot[at(i + 1, cj)] - G.du * 0.5 * (sxv[at(i + 1, cj)] + sxv[at(i, cj)]);
            for (int i = i0; i <= i1; ++i)
            {
               for (int j = cj + 1; j <= j1; ++j)
                  pot[at(i, j)] = pot[at(i, j - 1)] + G.du * 0.5 * (syv[at(i, j - 1)] + syv[at(i, j)]);
               for (int j = cj - 1; j >= j0; --j)
                  pot[at(i, j)] = pot[at(i, j + 1)] - G.du * 0.5 * (syv[at(i, j + 1)] + syv[at(i, j)]);
            }
            for (int j = j0; j <= j1; ++j)
               for (int i = i0; i <= i1; ++i)
               {
                  const size_t q = static_cast<size_t>(j) * bw + i;
                  if (!needRegions || lab[q] == R)
                     phi[q] = pot[at(i, j)];
               }
         }
      }
      // Per point: its family, field per run, z factors.
      bool unpol = false;
      for (size_t q = 0; q < np; ++q)
         if (lit(q) && pts[q].coh && pts[q].pol < -180.0f && std::isnan(pts[q].polLab))
            unpol = true;
      const int runs = unpol ? 2 : 1;
      const double wRun = unpol ? 0.5 : 1.0;
      const double zA = P.anchorFocus ? pivot : 0.0;
      struct CP
      {
         int i, j, set;
         bool evan;
         double kzK, kapK;
         cd e[2][3];
      };
      struct IP
      {
         double ux, uy, w, sx, sy, tx, ty, decay;
         int set;
         bool evan;
      };
      std::vector<CP> cps;
      std::vector<IP> ips;
      auto setOf = [&](bool evan) -> int {
         for (size_t k = 0; k < sets.size(); ++k)
            if (sets[k].fam == FAM_ABS || (evan ? sets[k].fam == FAM_EVA : sets[k].fam == FAM_REL))
               return static_cast<int>(k);
         return -1;
      };
      const double du2 = G.du * G.du;
      std::vector<double> wIF(sets.size(), 0.0);   // the directions' weights: I x F
      for (int j = 0; j < bh; ++j)
         for (int i = 0; i < bw; ++i)
         {
            const size_t q = static_cast<size_t>(j) * bw + i;
            if (!lit(q))
               continue;
            const Pt& p = pts[q];
            const double ux = UX(i), uy = UY(j);
            double r = std::hypot(ux, uy);
            if (std::fabs(r - 1.0) < kUEdge)
               r = r < 1.0 ? 1.0 - kUEdge : 1.0 + kUEdge;
            const bool evan = r > 1.0;
            const int set = setOf(evan);
            if (set < 0)
               continue;
            const double phiAz = (ux == 0.0 && uy == 0.0) ? 0.0 : std::atan2(uy, ux);
            double polDeg = p.pol;
            if (!std::isnan(p.polLab))
            {
               const double a = p.polLab * kDeg, g = BeamFrameSign(phiAz);
               polDeg = std::atan2(g * std::cos(a - phiAz), g * std::sin(a - phiAz)) / kDeg;
            }
            const bool upt = polDeg < -180.0;
            Beam b;
            b.ux = ux;
            b.uy = uy;
            b.amp = 1.0;
            b.polDeg = polDeg;
            b.ellipTurns = p.ellip;
            // Unpolarized: the mean of two runs polarized along the lab's x and y in the pupil (a spatially uniform
            // basis: per point the same intensity as its s and p runs', and coherent points add as a laser's do).
            cd E[2][3];
            double Fi = 0.0;
            for (int r2 = 0; r2 < (upt ? 2 : 1); ++r2)
            {
               if (upt)
               {
                  const double a = r2 == 0 ? 0.0 : kPi / 2, g = BeamFrameSign(phiAz);
                  b.polDeg = std::atan2(g * std::cos(a - phiAz), g * std::sin(a - phiAz)) / kDeg;
               }
               const BeamField f = BeamFieldOf(b, nm, P.nS, P.nI, P.na, 0);
               LabField(f, phiAz, E[r2][0], E[r2][1], E[r2][2]);
               Fi += (std::norm(E[r2][0]) + std::norm(E[r2][1]) + std::norm(E[r2][2])) * (upt ? 0.5 : 1.0);
            }
            const double kz = evan ? 0.0 : std::sqrt(1.0 - r * r), kap = evan ? std::sqrt(r * r - 1.0) : 0.0;
            F.power[static_cast<size_t>(set)] += p.I * du2;
            F.powerAll += p.I * du2;
            if (evan)
               F.powerEva += p.I * du2;
            if (!evan)
            {
               const double tn = r / kz, cphi = r > 0 ? ux / r : 1.0, sphi = r > 0 ? uy / r : 0.0;
               F.sx[static_cast<size_t>(set)] += p.I * Fi * tn * cphi;
               F.sy[static_cast<size_t>(set)] += p.I * Fi * tn * sphi;
               wIF[static_cast<size_t>(set)] += p.I * Fi;
            }
            if (p.coh)
            {
               CP c;
               c.i = i;
               c.j = j;
               c.set = set;
               c.evan = evan;
               c.kzK = K * kz;
               c.kapK = K * kap;
               // The phase: the map's, the landing point's potential, the anchor (the region's centre in phase at zA).
               double ph = 2.0 * kPi * p.phase - K * phi[q];
               if (needRegions && lab[q] >= 0 && !P.anchorFocus)
               {
                  const int R = lab[q];
                  const double rc = std::hypot(cxR[static_cast<size_t>(R)], cyR[static_cast<size_t>(R)]);
                  if (rc < 1.0)
                     ph += K * std::sqrt(1.0 - rc * rc) * (pivot - zA);
               }
               const cd amp = std::polar(std::sqrt(static_cast<double>(p.I)) * du2, ph);
               for (int r2 = 0; r2 < runs; ++r2)
                  for (int k = 0; k < 3; ++k)
                     c.e[r2][k] = amp * E[upt ? r2 : 0][k];
               cps.push_back(c);
            }
            else
            {
               IP ip;
               shiftOf(i, j, needRegions ? lab[q] : -1, ip.sx, ip.sy);
               ip.ux = ux;
               ip.uy = uy;
               ip.w = p.I * du2 * Fi;
               ip.evan = evan;
               ip.set = set;
               ip.tx = evan ? 0.0 : (r / kz) * (r > 0 ? ux / r : 1.0);
               ip.ty = evan ? 0.0 : (r / kz) * (r > 0 ? uy / r : 0.0);
               ip.decay = evan ? nm * 1e-3 / (4.0 * kPi * P.nS * kap) : 0.0;
               ips.push_back(ip);
            }
         }
      for (size_t k = 0; k < sets.size(); ++k)
         if (wIF[k] > 0)
         {
            F.sx[k] /= wIF[k];
            F.sy[k] /= wIF[k];
         }

      // The transforms: x over the box's columns, then y over its rows.
      Dft1 tx, ty;
      tx.Init(bw, K * UX(0), K * G.du, nxf, xf0, dx);
      ty.Init(bh, K * UY(0), K * G.du, nxf, xf0, dx);
      // Which (run, component) carry light, and which rows, per set.
      for (size_t k = 0; k < sets.size(); ++k)
      {
         const PlaneSet& ps = sets[k];
         const int nz = static_cast<int>(ps.z.size());
         std::vector<float>& planes = F.fam[k];
         planes.assign(static_cast<size_t>(nz) * nxf * nxf, 0.0f);
         std::vector<const CP*> mine;
         for (const CP& c : cps)
            if (c.set == static_cast<int>(k))
               mine.push_back(&c);
         std::vector<const IP*> inc;
         for (const IP& p : ips)
            if (p.set == static_cast<int>(k))
               inc.push_back(&p);
         bool comp[2][3] = {};
         std::vector<uint8_t> rowUsed(static_cast<size_t>(bh), 0);
         for (const CP* c : mine)
         {
            rowUsed[static_cast<size_t>(c->j)] = 1;
            for (int r2 = 0; r2 < runs; ++r2)
               for (int cc = 0; cc < 3; ++cc)
                  if (std::norm(c->e[r2][cc]) > 0)
                     comp[r2][cc] = true;
         }
         // Incoherent: bins of points (at most kIncoherentBins), each the footprint at its landing point.
         struct Bin
         {
            double w = 0, ux = 0, uy = 0, sx = 0, sy = 0, tx = 0, ty = 0, dInv = 0;
         };
         std::vector<Bin> bins;
         if (!inc.empty())
         {
            const int b = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(inc.size()) / kIncoherentBins))));
            std::map<long long, Bin> m;
            for (const IP* p : inc)
            {
               const long long key = (static_cast<long long>(std::floor(p->uy / (b * G.du))) << 32) ^
                                     static_cast<long long>(static_cast<uint32_t>(static_cast<int>(std::floor(p->ux / (b * G.du)))));
               Bin& B = m[key];
               B.w += p->w;
               B.ux += p->w * p->ux;
               B.uy += p->w * p->uy;
               B.sx += p->w * p->sx;
               B.sy += p->w * p->sy;
               B.tx += p->w * p->tx;
               B.ty += p->w * p->ty;
               B.dInv += p->w * (p->decay > 0 ? 1.0 / p->decay : 0.0);
            }
            for (auto& e : m)
            {
               Bin B = e.second;
               if (!(B.w > 0))
                  continue;
               B.ux /= B.w;
               B.uy /= B.w;
               B.sx /= B.w;
               B.sy /= B.w;
               B.tx /= B.w;
               B.ty /= B.w;
               B.dInv /= B.w;
               bins.push_back(B);
            }
         }
         const double dxi = std::isfinite(env.scale) ? std::max(dx, std::min(kMaxDxUm, env.scale / 3.0)) : kMaxDxUm;
         const int nxi = 2 * static_cast<int>(std::ceil((xf0 < 0 ? -xf0 : xf0) / dxi)) + 1;
         const double xi0 = -(nxi - 1) / 2 * dxi;
         const double incScale = (2.0 * kPi / K) * (2.0 * kPi / K) / env.integral;
         ParallelFor(static_cast<unsigned>(nz), [&](unsigned kz) {
            const double zp = ps.z[kz];
            // zeta from the focal plane (propagating phases), z from the coverslip (evanescent decays).
            const double zeta = ps.fam == FAM_REL ? zp : zp - pivot;
            const double zabs = ps.fam == FAM_REL ? zp + pivot : zp;
            float* plane = planes.data() + static_cast<size_t>(kz) * nxf * nxf;
            if (!mine.empty())
            {
               std::vector<cd> in(static_cast<size_t>(bw) * bh), G1(static_cast<size_t>(nxf) * bh), col(static_cast<size_t>(bh)),
                  outc(static_cast<size_t>(nxf)), row(static_cast<size_t>(nxf));
               std::vector<cd> zf(mine.size());
               for (size_t m = 0; m < mine.size(); ++m)
               {
                  const CP& c = *mine[m];
                  zf[m] = c.evan ? cd(std::exp(-c.kapK * std::max(0.0, zabs)), 0.0) : std::polar(1.0, c.kzK * zeta);
               }
               Dft1::Scratch sc;
               for (int r2 = 0; r2 < runs; ++r2)
                  for (int cc = 0; cc < 3; ++cc)
                  {
                     if (!comp[r2][cc])
                        continue;
                     std::fill(in.begin(), in.end(), cd(0.0, 0.0));
                     for (size_t m = 0; m < mine.size(); ++m)
                     {
                        const CP& c = *mine[m];
                        in[static_cast<size_t>(c.j) * bw + c.i] = c.e[r2][cc] * zf[m];
                     }
                     // Along x per used row: G1[p][row].
                     std::fill(G1.begin(), G1.end(), cd(0.0, 0.0));
                     for (int j = 0; j < bh; ++j)
                     {
                        if (!rowUsed[static_cast<size_t>(j)])
                           continue;
                        tx.Apply(in.data() + static_cast<size_t>(j) * bw, row.data(), sc);
                        for (int p = 0; p < nxf; ++p)
                           G1[static_cast<size_t>(p) * bh + j] = row[static_cast<size_t>(p)];
                     }
                     // Along y per column p.
                     for (int p = 0; p < nxf; ++p)
                     {
                        ty.Apply(G1.data() + static_cast<size_t>(p) * bh, outc.data(), sc);
                        for (int q = 0; q < nxf; ++q)
                           plane[static_cast<size_t>(q) * nxf + p] += static_cast<float>(wRun * std::norm(outc[static_cast<size_t>(q)]));
                     }
                  }
            }
            if (!bins.empty())
            {
               std::vector<float> coarse(static_cast<size_t>(nxi) * nxi, 0.0f);
               for (const Bin& B : bins)
               {
                  const double depth = B.dInv > 0 ? std::exp(-std::max(0.0, zabs) * B.dInv) : 1.0;
                  const double cx = B.sx + (B.dInv > 0 ? 0.0 : zeta * B.tx), cy = B.sy + (B.dInv > 0 ? 0.0 : zeta * B.ty);
                  const double w = B.w * depth;
                  if (!(w > 0))
                     continue;
                  const double reach = std::isfinite(env.scale) ? env.Reach(1e-6) : 1e18;
                  const int ia = std::max(0, static_cast<int>(std::floor((cx - reach - xi0) / dxi)));
                  const int ib = std::min(nxi - 1, static_cast<int>(std::ceil((cx + reach - xi0) / dxi)));
                  const int ja = std::max(0, static_cast<int>(std::floor((cy - reach - xi0) / dxi)));
                  const int jb = std::min(nxi - 1, static_cast<int>(std::ceil((cy + reach - xi0) / dxi)));
                  for (int jj = ja; jj <= jb; ++jj)
                  {
                     const double y = xi0 + jj * dxi - cy;
                     for (int ii = ia; ii <= ib; ++ii)
                     {
                        const double x = xi0 + ii * dxi - cx;
                        coarse[static_cast<size_t>(jj) * nxi + ii] += static_cast<float>(w * env.At(x * x + y * y));
                     }
                  }
               }
               for (int q = 0; q < nxf; ++q)
                  for (int p = 0; p < nxf; ++p)
                     plane[static_cast<size_t>(q) * nxf + p] += static_cast<float>(
                        incScale * std::max(0.0, Bicubic(coarse.data(), nxi, nxi, xi0, xi0, dxi, xf0 + p * dx, xf0 + q * dx)));
            }
         });
      }
   }

   bool Build()
   {
      const auto t0 = std::chrono::steady_clock::now();
      if (!Setup())
         return false;
      out.slots_ = S;
      out.period_ = period;
      std::vector<double> lines = P.lines;
      if (lines.empty())
         lines.push_back(P.lambdaRefNm);
      std::vector<double> scratch;
      bool anyEvan = false, anyProp = false;
      double powEva = 0, powAll = 0;   // every line's and slot's pupil power, evanescent and in all
      double uOuter = 0, decayPerNm = 0, tanS = 0;
      char buf[512];
      std::string info;
      // First pass over every line: pivot dependence (one coherent field with propagating and evanescent light, or
      // several propagating regions in phase at the coverslip).
      struct LineA
      {
         double nm;
         Analysis A;
         std::vector<std::vector<Pt>> raster;   // per evaluation slot
      };
      std::vector<LineA> la;
      const int evalSlots = tdep || timing ? S : 1;
      for (double nm : lines)
      {
         LineA L;
         L.nm = nm;
         SetVars(nm);
         L.raster.resize(static_cast<size_t>(evalSlots));
         for (int s = 0; s < evalSlots; ++s)
            AnalyseSlot(nm, SlotT(s), L.raster[static_cast<size_t>(s)], L.A, true, scratch);
         for (int s = 0; s < evalSlots; ++s)
            AnalyseSlot(nm, SlotT(s), L.raster[static_cast<size_t>(s)], L.A, false, scratch);
         for (int s = 0; s < evalSlots; ++s)
            AnalyseSlot2(nm, L.raster[static_cast<size_t>(s)], L.A);
         if (L.A.mixed || (!P.anchorFocus && L.A.propRegions > 1))
            out.pivotDependent_ = true;
         {
            // The steepness below which all but kReachPower of the propagating power lies (the grid's reach).
            double tot = 0, above = 0;
            for (double w : L.A.tanW)
               tot += w;
            for (int b = kTanBins; tot > 0 && b > 0; --b)
            {
               above += L.A.tanW[static_cast<size_t>(b)];
               if (above > kReachPower * tot)
               {
                  L.A.tanMax = std::min(L.A.tanMax, (b + 1) * 50.0 / kTanBins);
                  break;
               }
            }
         }
         la.push_back(std::move(L));
      }
      for (LineA& L : la)
      {
         const double nm = L.nm;
         Analysis& A = L.A;
         SetVars(nm);
         BfpLight::Line line;
         line.nm = nm;
         const double K = 2.0 * kPi * P.nS / (nm * 1e-3);
         if (!(A.Imax > 0))
         {
            line.states.assign(static_cast<size_t>(period), BfpLightField());
            out.lines_.push_back(std::move(line));
            continue;
         }
         anyEvan = anyEvan || A.evan;
         anyProp = anyProp || A.prop;
         uOuter = std::max(uOuter, A.uOuter);
         if (A.evan)
            decayPerNm = decayPerNm > 0 ? std::min(decayPerNm, A.dMin / nm) : A.dMin / nm;
         const double sbx = A.wsum > 0 ? A.sbx / A.wsum : 0.0, sby = A.wsum > 0 ? A.sby / A.wsum : 0.0;
         if (tanS == 0.0)
            tanS = std::hypot(sbx, sby);
         // The lateral grid.
         double dx = kMaxDxUm;
         if (A.span > 0)
            dx = std::min(dx, 2.0 * kPi / (6.0 * K * A.span));
         if (A.incoh && std::isfinite(env.scale))
            dx = std::min(dx, env.scale / 3.0);
         int nxh = static_cast<int>(std::ceil(Wh / dx));
         if (2 * nxh + 1 > kMaxSide)
         {
            nxh = (kMaxSide - 1) / 2;
            dx = Wh / nxh;
         }
         const int nx = 2 * nxh + 1;
         const double x0 = -nxh * dx;
         // The plane sets (z grids).
         std::vector<PlaneSet> sets;
         const double zs = std::max(0.5, P.zSampleUm);
         double dzP = A.dg > 0 ? std::min(0.5, std::max(0.01, kPi / (3.0 * A.dg))) : 0.5;
         if (A.devMax > 0 && std::isfinite(env.scale))
            dzP = std::min(dzP, std::max(0.01, 0.3 * env.scale / A.devMax));
         const double dzE = A.evan ? std::min(0.5, std::max(kZSnap, A.dMin / 6.0)) : 0.5;
         const double topE = A.evan ? std::min(zs, 8.0 * A.dMax) : 0.0;
         auto grid = [&](double a, double b, double dz, std::vector<double>& z) {
            const int n = std::max(1, static_cast<int>(std::ceil((b - a) / dz)));
            for (int k = 0; k <= n; ++k)
               z.push_back(Snap(a + (b - a) * k / n));
         };
         auto finish = [&](PlaneSet& ps) {
            std::sort(ps.z.begin(), ps.z.end());
            ps.z.erase(std::unique(ps.z.begin(), ps.z.end(), [](double a, double b) { return std::fabs(a - b) < kZSnap / 2; }),
                       ps.z.end());
            while (static_cast<int>(ps.z.size()) > kMaxPlanes)
            {
               std::vector<double> t;
               for (size_t k = 0; k < ps.z.size(); k += 2)
                  t.push_back(ps.z[k]);
               if (t.back() != ps.z.back())
                  t.push_back(ps.z.back());
               ps.z.swap(t);
            }
         };
         if (out.pivotDependent_)
         {
            PlaneSet ps;
            ps.fam = FAM_ABS;
            if (A.evan)
               grid(0.0, std::max(kZSnap, topE), dzE, ps.z);
            if (A.prop)
               grid(A.evan ? topE : 0.0, zs, dzP, ps.z);
            ps.extendTop = A.prop;
            finish(ps);
            sets.push_back(ps);
         }
         else
         {
            if (A.prop)
            {
               PlaneSet ps;
               ps.fam = FAM_REL;
               grid(-zs, zs, dzP, ps.z);
               finish(ps);
               sets.push_back(ps);
            }
            if (A.evan)
            {
               PlaneSet ps;
               ps.fam = FAM_EVA;
               grid(0.0, std::max(kZSnap, topE), dzE, ps.z);
               ps.extendTop = false;
               finish(ps);
               sets.push_back(ps);
            }
         }
         // The pupil grid: its period 2 pi / (k du) beyond every place the light reaches, plus the window.
         double rot = 0, dxs = 0, dys = 0;
         bool anyRot = false;
         double shiftScan = 0;
         std::vector<double> sRot(static_cast<size_t>(S)), sDx(sRot.size()), sDy(sRot.size());
         for (int s = 0; s < S; ++s)
         {
            if (scans)
               ScanOf(s, rot, dxs, dys, scratch);
            sRot[static_cast<size_t>(s)] = rot;
            sDx[static_cast<size_t>(s)] = dxs;
            sDy[static_cast<size_t>(s)] = dys;
            anyRot = anyRot || std::fabs(std::remainder(rot, 2.0 * kPi)) > 1e-12;
            shiftScan = std::max(shiftScan, std::hypot(dxs, dys));
         }
         const double Wf = Wh * (anyRot ? std::sqrt(2.0) : 1.0) + shiftScan;
         const double reachZ = A.prop ? std::min(200.0, zs * std::min(A.tanMax, 50.0)) : 0.0;
         const double E = std::max(4.3 / (K * 2.0 * A.sigMin), A.incoh ? 0.0 : 0.0);
         const double rho = A.shiftMax + reachZ + E;
         const double L1 = 1.1 * (rho + Wf + 2.0 * dx);
         Grid G;
         G.du = 2.0 * kPi / (K * L1);
         // No coherent light: no field's period to keep beyond the window; the map's own detail is enough.
         if (!A.coh)
            G.du = std::max(G.du, 2.0 * umax / kIncoherentPupil);
         int half = static_cast<int>(std::ceil(umax / G.du));
         bool capped = false;
         if (2 * half + 1 > kMaxPupil)
         {
            half = (kMaxPupil - 1) / 2;
            G.du = umax / half;
            capped = true;
         }
         G.M = 2 * half + 1;
         const double a = 2.0 * umax / kAnalysisPx;
         G.mx0 = std::max(0, static_cast<int>(std::floor((A.ux0 - 2 * a) / G.du)) + half);
         G.mx1 = std::min(G.M - 1, static_cast<int>(std::ceil((A.ux1 + 2 * a) / G.du)) + half);
         G.my0 = std::max(0, static_cast<int>(std::floor((A.uy0 - 2 * a) / G.du)) + half);
         G.my1 = std::min(G.M - 1, static_cast<int>(std::ceil((A.uy1 + 2 * a) / G.du)) + half);
         const int bw = G.mx1 - G.mx0 + 1, bh = G.my1 - G.my0 + 1;
         const int nxf = 2 * static_cast<int>(std::ceil(Wf / dx)) + 1;
         // The points on the field grid: once (nothing but the scans and the timing depends on t), else per slot.
         std::vector<std::vector<Pt>> fpts(static_cast<size_t>(tdep ? S : 1));
         for (size_t e = 0; e < fpts.size(); ++e)
         {
            const double t = SlotT(static_cast<int>(e));
            fpts[e].resize(static_cast<size_t>(bw) * bh);
            std::vector<Pt>& v = fpts[e];
            ParallelFor(static_cast<unsigned>(bh), [&](unsigned j) {
               std::vector<double> sc;
               for (int i = 0; i < bw; ++i)
                  v[static_cast<size_t>(j) * bw + i] = PointAt(G.U(G.mx0 + i), G.U(G.my0 + static_cast<int>(j)), t, sc);
            });
         }
         // The slots' lit masks; slots with the same points and mask share a field.
         std::vector<std::vector<uint8_t>> masks;
         std::vector<int> fieldOf(static_cast<size_t>(S));
         std::vector<int> fieldPts;
         std::map<std::pair<int, uint64_t>, int> seen;
         double rawPower = 0.0;
         for (int s = 0; s < S; ++s)
         {
            const int e = tdep ? s : 0;
            const std::vector<Pt>& v = fpts[static_cast<size_t>(e)];
            std::vector<uint8_t> m(v.size());
            const double t = SlotT(s);
            for (size_t q = 0; q < v.size(); ++q)
               m[q] = v[q].I > 0 && LitAt(t, v[q].T, v[q].duty) ? 1 : 0;
            for (size_t q = 0; q < v.size(); ++q)
               if (m[q])
                  rawPower += v[q].I * G.du * G.du;
            const uint64_t h = BfpHashBytes(m.data(), m.size());
            auto it = seen.find({ e, h });
            if (it != seen.end())
            {
               fieldOf[static_cast<size_t>(s)] = it->second;
               continue;
            }
            const int id = static_cast<int>(masks.size());
            masks.push_back(std::move(m));
            fieldPts.push_back(e);
            seen[{ e, h }] = id;
            fieldOf[static_cast<size_t>(s)] = id;
         }
         rawPower /= S;
         // The reference power: the legacy epi beam's, (k / 2 pi)^2 x its envelope's integral (Parseval).
         const double piRef = (K / (2.0 * kPi)) * (K / (2.0 * kPi)) * env.integral;
         const double cNorm = rawPower > 0 ? piRef / rawPower : 0.0;
         // The states' volumes.
         auto newVolume = [&](const PlaneSet& ps) {
            BfpVolume v;
            v.x0 = x0;
            v.y0 = x0;
            v.dx = dx;
            v.nx = nx;
            v.ny = nx;
            v.z = ps.z;
            v.relative = ps.fam == FAM_REL;
            v.extendTop = ps.extendTop;
            v.extendBottom = ps.extendBottom;
            v.v.assign(static_cast<size_t>(ps.z.size()) * nx * nx, 0.0f);
            return v;
         };
         line.states.resize(static_cast<size_t>(period));
         std::vector<int> slotsInState(static_cast<size_t>(period), 0);
         for (int s = 0; s < S; ++s)
            slotsInState[static_cast<size_t>(StateOf(s))]++;
         for (BfpLightField& st : line.states)
            for (const PlaneSet& ps : sets)
               st.parts.push_back(newVolume(ps));
         std::vector<std::vector<double>> shearW(static_cast<size_t>(period), std::vector<double>(sets.size() * 3, 0.0));
         const double pivot = P.pivotUm;
         size_t fieldsMade = 0;
         for (size_t f = 0; f < masks.size(); ++f)
         {
            FieldPlanes FP;
            const int e = fieldPts[f];
            ComputeField(nm, G, fpts[static_cast<size_t>(e)], masks[f], A.Imax, sets, nxf, dx, pivot, FP);
            fieldsMade++;
            // Slots that only shift this field (no rotation) add up to one sparse kernel of bicubic taps per state and
            // plane set, convolved once (a scan of S slots: S x 16 taps per pixel became the kernel's distinct taps,
            // a line scan's ~4 x its length): the same sum, negative ringing clipped from the sum, not per slot.
            std::vector<std::map<std::pair<int, int>, double>> shiftK(static_cast<size_t>(period) * sets.size());
            for (int s = 0; s < S; ++s)
               if (fieldOf[static_cast<size_t>(s)] == static_cast<int>(f))
               {
                  powEva += FP.powerEva;
                  powAll += FP.powerAll;
               }
            for (int s = 0; s < S; ++s)
            {
               if (fieldOf[static_cast<size_t>(s)] != static_cast<int>(f))
                  continue;
               const int st = StateOf(s);
               const double w = 1.0 / std::max(1, slotsInState[static_cast<size_t>(st)]);
               const double th = sRot[static_cast<size_t>(s)], c = std::cos(th), sn = std::sin(th);
               const double ddx = sDx[static_cast<size_t>(s)], ddy = sDy[static_cast<size_t>(s)];
               const bool identity = std::fabs(std::remainder(th, 2.0 * kPi)) < 1e-12 && ddx == 0.0 && ddy == 0.0 && nxf == nx;
               for (size_t k = 0; k < sets.size(); ++k)
               {
                  BfpVolume& V = line.states[static_cast<size_t>(st)].parts[k];
                  const std::vector<float>& src = FP.fam[k];
                  const int nz = V.Nz();
                  // The slot's direction (rotated) and its light, for the state's mean direction.
                  double pw = FP.power[k];
                  shearW[static_cast<size_t>(st)][3 * k] += w * pw * (c * FP.sx[k] - sn * FP.sy[k]);
                  shearW[static_cast<size_t>(st)][3 * k + 1] += w * pw * (sn * FP.sx[k] + c * FP.sy[k]);
                  shearW[static_cast<size_t>(st)][3 * k + 2] += w * pw;
                  if (identity)
                  {
                     for (size_t q = 0; q < V.v.size(); ++q)
                        V.v[q] += static_cast<float>(w * src[q]);
                     continue;
                  }
                  if (std::fabs(std::remainder(th, 2.0 * kPi)) < 1e-12)
                  {
                     // out(p, q) += w x sum of taps src(p + i - 1 + a, q + j - 1 + b), as Bicubic at X - d.
                     const double ox = (x0 - ddx - FP.x0) / dx, oy = (x0 - ddy - FP.x0) / dx;
                     const int i = static_cast<int>(std::floor(ox)), j = static_cast<int>(std::floor(oy));
                     double wx[4], wy[4];
                     CubicWeights(ox - i, wx);
                     CubicWeights(oy - j, wy);
                     auto& km = shiftK[static_cast<size_t>(st) * sets.size() + k];
                     for (int b = 0; b < 4; ++b)
                        for (int a = 0; a < 4; ++a)
                           if (wx[a] * wy[b] != 0.0)
                              km[{ i - 1 + a, j - 1 + b }] += w * wx[a] * wy[b];
                     continue;
                  }
                  ParallelFor(static_cast<unsigned>(nz), [&](unsigned kz) {
                     const float* sp = src.data() + static_cast<size_t>(kz) * nxf * nxf;
                     float* dp = V.v.data() + static_cast<size_t>(kz) * nx * nx;
                     for (int q = 0; q < nx; ++q)
                        for (int p = 0; p < nx; ++p)
                        {
                           const double X = x0 + p * dx, Y = x0 + q * dx;
                           // The light rotated by th after its shift: the source point R(-th) X - d.
                           const double xs = c * X + sn * Y - ddx, ys = -sn * X + c * Y - ddy;
                           const double val = Bicubic(sp, nxf, nxf, FP.x0, FP.x0, dx, xs, ys);
                           if (val > 0)
                              dp[static_cast<size_t>(q) * nx + p] += static_cast<float>(w * val);
                        }
                  });
               }
            }
            for (size_t sk = 0; sk < shiftK.size(); ++sk)
            {
               if (shiftK[sk].empty())
                  continue;
               std::vector<std::pair<std::pair<int, int>, double>> taps(shiftK[sk].begin(), shiftK[sk].end());
               const size_t k = sk % sets.size();
               BfpVolume& V = line.states[sk / sets.size()].parts[k];
               const std::vector<float>& src = FP.fam[k];
               ParallelFor(static_cast<unsigned>(V.Nz()), [&](unsigned kz) {
                  const float* sp = src.data() + static_cast<size_t>(kz) * nxf * nxf;
                  float* dp = V.v.data() + static_cast<size_t>(kz) * nx * nx;
                  std::vector<double> acc(static_cast<size_t>(nx) * nx, 0.0);
                  for (const auto& t : taps)
                  {
                     const int ax = t.first.first, ay = t.first.second;
                     const int p0 = std::max(0, -ax), p1 = std::min(nx, nxf - ax);
                     for (int q = std::max(0, -ay); q < std::min(nx, nxf - ay); ++q)
                     {
                        const float* r = sp + static_cast<size_t>(q + ay) * nxf + ax;
                        double* o = acc.data() + static_cast<size_t>(q) * nx;
                        for (int p = p0; p < p1; ++p)
                           o[p] += t.second * r[p];
                     }
                  }
                  for (size_t q = 0; q < acc.size(); ++q)
                     if (acc[q] > 0)
                        dp[q] += static_cast<float>(acc[q]);
               });
            }
         }
         // Scale to the reference power; the states' directions; the cycle mean.
         line.mean.parts.clear();
         for (const PlaneSet& ps : sets)
            line.mean.parts.push_back(newVolume(ps));
         for (int st = 0; st < period; ++st)
            for (size_t k = 0; k < sets.size(); ++k)
            {
               BfpVolume& V = line.states[static_cast<size_t>(st)].parts[k];
               for (float& x : V.v)
                  x = static_cast<float>(x * cNorm);
               const auto& sw = shearW[static_cast<size_t>(st)];
               if (sets[k].fam != FAM_EVA && sw[3 * k + 2] > 0)
               {
                  V.sx = sw[3 * k] / sw[3 * k + 2];
                  V.sy = sw[3 * k + 1] / sw[3 * k + 2];
               }
               BfpVolume& Mv = line.mean.parts[k];
               for (size_t q = 0; q < V.v.size(); ++q)
                  Mv.v[q] += V.v[q] / static_cast<float>(period);
               Mv.sx += V.sx / period;
               Mv.sy += V.sy / period;
            }
         if (period == 1)
         {
            line.mean = line.states[0];
            line.states.clear();
         }
         // The mean light's curvature (the light history's lattice): its plane nearest the focus / the coverslip.
         for (const BfpVolume& V : line.mean.parts)
         {
            const int nz = V.Nz();
            int kb = 0;
            for (int k = 0; k < nz; ++k)
               if (std::fabs(V.z[static_cast<size_t>(k)]) < std::fabs(V.z[static_cast<size_t>(kb)]))
                  kb = k;
            const float* pl = V.v.data() + static_cast<size_t>(kb) * V.nx * V.ny;
            double peak = 0, cmax = 0;
            for (size_t q = 0; q < static_cast<size_t>(V.nx) * V.ny; ++q)
               peak = std::max(peak, static_cast<double>(pl[q]));
            if (peak > 0)
               for (int q = 1; q + 1 < V.ny; ++q)
                  for (int p = 1; p + 1 < V.nx; ++p)
                  {
                     const size_t c = static_cast<size_t>(q) * V.nx + p;
                     const double ax = std::fabs(pl[c + 1] - 2.0 * pl[c] + pl[c - 1]);
                     const double ay = std::fabs(pl[c + V.nx] - 2.0 * pl[c] + pl[c - V.nx]);
                     cmax = std::max(cmax, std::max(ax, ay) / (V.dx * V.dx) / peak);
                  }
            out.curvature_ = std::max(out.curvature_, cmax);
         }
         // How far from the axis the mean light reaches (down to 1e-4 of its peak, any plane): the light history's
         // footprints span this, not the whole field window.
         {
            double peak = 0;
            for (const BfpVolume& V : line.mean.parts)
               for (float v : V.v)
                  peak = std::max(peak, static_cast<double>(v));
            for (const BfpVolume& V : line.mean.parts)
               for (int k = 0; peak > 0 && k < V.Nz(); ++k)
               {
                  const float* pl = V.v.data() + static_cast<size_t>(k) * V.nx * V.ny;
                  for (int q = 0; q < V.ny; ++q)
                     for (int p = 0; p < V.nx; ++p)
                        if (pl[static_cast<size_t>(q) * V.nx + p] > 1e-4 * peak)
                           out.reachUm_ = std::max(out.reachUm_, std::hypot(V.x0 + (p + 1) * V.dx, V.y0 + (q + 1) * V.dx) );
               }
         }
         size_t planes = 0;
         for (const PlaneSet& ps : sets)
            planes += ps.z.size();
         std::snprintf(buf, sizeof(buf),
                       "%s%.0f nm: pupil %d x %d (du %.4g%s), window %.1f um at %.3g um (%d px), %zu planes, %zu field%s",
                       info.empty() ? "" : "; ", nm, bw, bh, G.du, capped ? ", capped: light may wrap" : "", 2.0 * Wh, dx,
                       nx, planes, fieldsMade, fieldsMade == 1 ? "" : "s");
         info += buf;
         out.lines_.push_back(std::move(line));
      }
      out.evanescent_ = anyEvan;
      out.propagating_ = anyProp;
      out.evanescentShare_ = powAll > 0 ? powEva / powAll : 0.0;
      out.uOuter_ = uOuter;
      out.decayPerNm_ = decayPerNm;
      out.tanS_ = tanS;
      out.halfWindowUm_ = Wh;
      out.computeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::snprintf(buf, sizeof(buf), "; %d slot%s, %d state%s, %.2f s, %.1f MB", S, S == 1 ? "" : "s", period,
                    period == 1 ? "" : "s", out.computeSec_, out.Bytes() / 1048576.0);
      out.info_ = info + buf;
      return true;
   }
};

namespace {
std::mutex& MemoMutex()
{
   static std::mutex m;
   return m;
}
std::list<std::pair<uint64_t, std::shared_ptr<const BfpLight>>>& Memo()
{
   static std::list<std::pair<uint64_t, std::shared_ptr<const BfpLight>>> l;
   return l;
}
constexpr size_t kMemo = 6;
#ifdef __EMSCRIPTEN__
constexpr size_t kMemoBytes = size_t(512) << 20;   // (the viewer's heap: 4 GB for everything)
#else
constexpr size_t kMemoBytes = size_t(2048) << 20;
#endif

uint64_t KeyOf(const BfpLightParams& p, const BfpProgram& prog, bool withPivot, std::string& err)
{
   uint64_t h = Mix(1469598103934665603ULL, p.text);
   for (double nm : p.lines)
      h = Mix(h, nm);
   for (double v : { p.lambdaRefNm, p.na, p.nS, p.nI, p.fObjUm, p.bfp, p.envFwhmUm, static_cast<double>(p.profile),
                     p.fovSpanUm, p.anchorFocus ? 1.0 : 0.0, p.sheetTUm, p.sheetWUm, static_cast<double>(p.slots),
                     static_cast<double>(p.cycleFrames), p.zSampleUm })
      h = Mix(h, v);
   for (const std::string& name : prog.Images())
   {
      std::string e;
      auto im = BfpFindImage(name, e);
      if (!im)
      {
         if (err.empty())
            err = e;
         continue;
      }
      h = Mix(h, static_cast<double>(im->hash));
   }
   if (withPivot)
      h = Mix(Mix(h, 7.0), std::round(p.pivotUm / 1e-4) * 1e-4);
   return h;
}
} // namespace

std::shared_ptr<const BfpLight> BfpLight::Make(const BfpLightParams& p, std::string& err)
{
   BfpProgram prog;
   if (!prog.Compile(p.text, err))
      return nullptr;
   const uint64_t k0 = KeyOf(p, prog, false, err);
   if (!err.empty())
      return nullptr;
   const uint64_t k1 = KeyOf(p, prog, true, err);
   {
      std::lock_guard<std::mutex> g(MemoMutex());
      for (auto it = Memo().begin(); it != Memo().end(); ++it)
         if ((it->first == k0 && !it->second->PivotDependent()) || (it->first == k1 && it->second->PivotDependent()))
         {
            auto r = it->second;
            Memo().splice(Memo().begin(), Memo(), it);
            return r;
         }
   }
   auto L = std::make_shared<BfpLight>();
   L->params_ = p;
   BfpBuilder b(p, *L);
   if (!b.Build())
   {
      err = b.err;
      return nullptr;
   }
   L->hash_ = L->pivotDependent_ ? k1 : k0;
   TimingLog("fl.bfp-maps", L->ComputeSec(), L->Info().c_str());
   std::lock_guard<std::mutex> g(MemoMutex());
   Memo().emplace_front(L->hash_, L);
   // At most kMemo lights and kMemoBytes of volumes (the newest always kept): a light-sheet's field is ~1 MB, a coherent
   // full pupil's or a lattice's 100-200 MB.
   size_t bytes = 0;
   for (const auto& m : Memo())
      bytes += m.second->Bytes();
   while (Memo().size() > kMemo || (Memo().size() > 1 && bytes > kMemoBytes))
   {
      bytes -= Memo().back().second->Bytes();
      Memo().pop_back();
   }
   return L;
}

namespace {
// The maps on an n x n raster over [-umax, umax]^2 (row 0 at +uy) at time t, shown where a scan turned them by rot: the
// point R(-rot) X; I is 0 outside the pupil and where the timing leaves it dark. Rows on threads.
void MapsRaster(const BfpBuilder& b, int n, double t, double rot, std::vector<std::vector<float>>& maps)
{
   maps.assign(kBfpMaps, std::vector<float>(static_cast<size_t>(n) * n, 0.0f));
   const double um = b.umax, c = std::cos(rot), sn = std::sin(rot);
   ParallelFor(static_cast<unsigned>(n), [&](unsigned jj) {
      const int j = static_cast<int>(jj);
      std::vector<double> scratch;
      for (int i = 0; i < n; ++i)
      {
         const double X = -um + (i + 0.5) * 2.0 * um / n, Y = um - (j + 0.5) * 2.0 * um / n;
         const double ux = c * X + sn * Y, uy = -sn * X + c * Y;
         double m[kBfpMaps];
         b.ev.Eval(ux, uy, t, m, scratch);
         if (std::hypot(ux, uy) > um || !LitAt(t, m[BM_T], m[BM_DUTY]) || !(m[BM_I] > 0))
            m[BM_I] = 0.0;
         for (int k = 0; k < kBfpMaps; ++k)
            maps[static_cast<size_t>(k)][static_cast<size_t>(j) * n + i] = static_cast<float>(m[k]);
      }
   });
}
} // namespace

bool BfpLight::Rasters(const BfpLightParams& p, int n, int slot, double nm, std::vector<std::vector<float>>& maps,
                       std::string& err)
{
   BfpLight tmp;
   BfpBuilder b(p, tmp);
   if (!b.Setup())
   {
      err = b.err;
      return false;
   }
   b.SetVars(nm > 0 ? nm : p.lambdaRefNm);
   const int s = std::max(0, std::min(slot, b.S - 1));
   const double t = b.SlotT(s);
   std::vector<double> scratch;
   double rot = 0, dxs = 0, dys = 0;
   if (b.scans)
      b.ScanOf(s, rot, dxs, dys, scratch);
   MapsRaster(b, n, t, rot, maps);
   return true;
}

bool BfpLight::RasterSum(const BfpLightParams& p, int n, const std::vector<int>& slots, double nm,
                         std::vector<float>& sumI, std::vector<std::vector<float>>& first, std::string& err)
{
   sumI.assign(static_cast<size_t>(n) * n, 0.0f);
   first.clear();
   if (slots.empty())
      return true;
   BfpLight tmp;
   BfpBuilder b(p, tmp);
   if (!b.Setup())
   {
      err = b.err;
      return false;
   }
   b.SetVars(nm > 0 ? nm : p.lambdaRefNm);
   std::vector<double> scratch;
   auto scanOf = [&](int s, double& rot) {
      double dxs = 0, dys = 0;
      rot = 0;
      if (b.scans)
         b.ScanOf(std::max(0, std::min(s, b.S - 1)), rot, dxs, dys, scratch);   // (a shift tilts the light: the pupil stays)
   };
   double rot0 = 0;
   scanOf(slots[0], rot0);
   MapsRaster(b, n, b.SlotT(std::max(0, std::min(slots[0], b.S - 1))), rot0, first);
   if (b.tdep || b.timing)
   {
      // Maps that change with t: each slot drawn on its own.
      std::vector<std::vector<float>> m;
      for (size_t k = 0; k < slots.size(); ++k)
      {
         double rot = 0;
         scanOf(slots[k], rot);
         const std::vector<std::vector<float>>* src = &first;
         if (k > 0)
         {
            MapsRaster(b, n, b.SlotT(std::max(0, std::min(slots[k], b.S - 1))), rot, m);
            src = &m;
         }
         const std::vector<float>& I = (*src)[BM_I];
         for (size_t q = 0; q < sumI.size(); ++q)
            sumI[q] += I[q];
      }
      return true;
   }
   // Maps that do not change with t (a scan at most): one drawing, unturned, turned per slot (bilinear; the light's
   // own shortcut for a rotation scan, BfpLight::Make).
   std::vector<std::vector<float>> base;
   MapsRaster(b, n, b.SlotT(0), 0.0, base);
   const std::vector<float>& B = base[BM_I];
   const double um = b.umax;
   for (size_t k = 0; k < slots.size(); ++k)
   {
      double rot = 0;
      scanOf(slots[k], rot);
      if (std::fabs(std::remainder(rot, 2.0 * kPi)) < 1e-12)
      {
         for (size_t q = 0; q < sumI.size(); ++q)
            sumI[q] += B[q];
         continue;
      }
      const double c = std::cos(rot), sn = std::sin(rot);
      ParallelFor(static_cast<unsigned>(n), [&](unsigned jj) {
         const int j = static_cast<int>(jj);
         for (int i = 0; i < n; ++i)
         {
            const double X = -um + (i + 0.5) * 2.0 * um / n, Y = um - (j + 0.5) * 2.0 * um / n;
            const double ux = c * X + sn * Y, uy = -sn * X + c * Y;
            const double fi = (ux + um) / (2.0 * um) * n - 0.5, fj = (um - uy) / (2.0 * um) * n - 0.5;
            const int i0 = static_cast<int>(std::floor(fi)), j0 = static_cast<int>(std::floor(fj));
            const double ax = fi - i0, ay = fj - j0;
            auto at = [&](int x, int y) {
               return x >= 0 && y >= 0 && x < n && y < n ? static_cast<double>(B[static_cast<size_t>(y) * n + x]) : 0.0;
            };
            sumI[static_cast<size_t>(j) * n + i] += static_cast<float>(
               (1 - ay) * ((1 - ax) * at(i0, j0) + ax * at(i0 + 1, j0)) + ay * ((1 - ax) * at(i0, j0 + 1) + ax * at(i0 + 1, j0 + 1)));
         }
      });
   }
   return true;
}

} // namespace sim
