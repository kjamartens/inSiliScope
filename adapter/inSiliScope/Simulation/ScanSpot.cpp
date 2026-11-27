///////////////////////////////////////////////////////////////////////////////
// FILE:          ScanSpot.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Confocal laser scanning (CSLM) excitation: the focused spot's
//                tables, the scan's windows and the scan light (ScanSpot.h).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "ScanSpot.h"

#include "BfpField.h"
#include "BfpMaps.h"
#include "Fft2d.h"
#include "Parallel.h"
#include "Timing.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <list>
#include <mutex>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Numerical Debye up to this many radians of k |z| (1 - cos thx) (~6 axial periods); beyond, the geometric limit.
constexpr double kDebyeAxialRad = 40.0;
constexpr double kRayShare = 0.99;       // a plane's reach: this share of the rays' power, plus kTailLambdas lambda / NA
constexpr double kTailLambdas = 8.0;
constexpr int kTableHalf = 96;           // a table's samples per reach (at most; the in-focus plane lambda / (8 NA))
constexpr int kFine = 4;                 // the integration's sub-steps per table step
constexpr double kPlaneGrowth = 0.08;    // the planes' spacing grows by this share of |z|
constexpr int kMaxPlanes = 96;

// Abramowitz & Stegun 9.4.1 and 9.4.3 (|error| < 5e-8 / 1.6e-8): J0 (WASM's libc++ has no special functions).
double BesselJ0(double x)
{
   x = std::fabs(x);
   if (x <= 3.0)
   {
      const double t = (x / 3.0) * (x / 3.0);
      return 1.0 + t * (-2.2499997 + t * (1.2656208 + t * (-0.3163866 + t * (0.0444479 + t * (-0.0039444 + t * 0.0002100)))));
   }
   const double u = 3.0 / x;
   const double f0 = 0.79788456 + u * (-0.00000077 + u * (-0.00552740 + u * (-0.00009512 + u * (0.00137237 +
                     u * (-0.00072805 + u * 0.00014476)))));
   const double t0 = x - 0.78539816 + u * (-0.04166397 + u * (-0.00003954 + u * (0.00262573 + u * (-0.00054125 +
                     u * (-0.00029333 + u * 0.00013558)))));
   return f0 * std::cos(t0) / std::sqrt(x);
}

const double kGx[8] = { -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                        0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363 };
const double kGw[8] = { 0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                        0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763 };

void CubicW(double t, double w[4])
{
   const double t2 = t * t, t3 = t2 * t;
   w[0] = -0.5 * t3 + t2 - 0.5 * t;
   w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
   w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
   w[3] = 0.5 * t3 - 0.5 * t2;
}

int Clampi(int i, int lo, int hi) { return i < lo ? lo : (i > hi ? hi : i); }

// Catmull-Rom on a grid of ny rows x nx columns, at column fx, row fy (sample units); the taps clamped to the grid
// (rows mirrored about 0 first when mirrorY).
double Tab2(const float* g, int nx, int ny, double fx, double fy, bool mirrorY)
{
   const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
   double wx[4], wy[4];
   CubicW(fx - i, wx);
   CubicW(fy - j, wy);
   double s = 0.0;
   for (int b = 0; b < 4; ++b)
   {
      int jj = j - 1 + b;
      if (mirrorY && jj < 0)
         jj = -jj;
      const float* row = g + static_cast<size_t>(Clampi(jj, 0, ny - 1)) * nx;
      double r = 0.0;
      for (int a = 0; a < 4; ++a)
         r += wx[a] * row[Clampi(i - 1 + a, 0, nx - 1)];
      s += wy[b] * r;
   }
   return s;
}

double Tab1(const float* g, int n, double f, bool mirror)
{
   const int i = static_cast<int>(std::floor(f));
   double w[4];
   CubicW(f - i, w);
   double s = 0.0;
   for (int a = 0; a < 4; ++a)
   {
      int ii = i - 1 + a;
      if (mirror && ii < 0)
         ii = -ii;
      s += w[a] * g[Clampi(ii, 0, n - 1)];
   }
   return s;
}

long FloorL(double v) { return static_cast<long>(std::floor(std::max(-1e9, std::min(1e9, v)))); }
long CeilL(double v) { return static_cast<long>(std::ceil(std::max(-1e9, std::min(1e9, v)))); }
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The scan's windows.

namespace {
// The runs of scan time [s0, s1) within one scan.
void SegmentRuns(const ScanProgram& g, double s0, double s1, std::vector<ScanRun>& runs)
{
   if (!(s1 > s0))
      return;
   const double tl = g.lineSec;
   int k0 = static_cast<int>(std::floor(s0 / tl));
   int k1 = static_cast<int>(std::floor(s1 / tl));
   double u1End = s1 - k1 * tl;
   if (k1 > k0 && !(u1End > 0.0))
   {
      // s1 on a line's start: the window ends with the line before.
      --k1;
      u1End = tl;
   }
   k0 = Clampi(k0, 0, g.N - 1);
   k1 = Clampi(k1, 0, g.N - 1);
   auto add = [&](int k1_, int k2_, double ua, double ub) {
      ua = std::max(0.0, ua);
      ub = std::min(g.sweepSec, ub);
      if (!(ub > ua) || k2_ < k1_)
         return;
      ScanRun r;
      r.k1 = k1_;
      r.k2 = k2_;
      r.xs = g.x0 + ua * g.v;
      r.xe = g.x0 + ub * g.v;
      r.w = 1.0;
      runs.push_back(r);
   };
   if (k0 == k1)
   {
      add(k0, k0, s0 - k0 * tl, s1 - k0 * tl);
      return;
   }
   add(k0, k0, s0 - k0 * tl, tl);
   if (k1 > k0 + 1)
      add(k0 + 1, k1 - 1, 0.0, tl);
   add(k1, k1, 0.0, u1End);
}
} // namespace

void ScanRunsOf(const ScanProgram& g, double t0, double E, std::vector<ScanRun>& runs)
{
   runs.clear();
   const double T = g.scanSec;
   if (!(T > 0.0) || !(E > 0.0))
      return;
   double nf = std::floor(E / T);
   double rem = E - nf * T;
   if (rem > T * (1.0 - 1e-9))
   {
      nf += 1.0;
      rem = 0.0;
   }
   if (rem < T * 1e-9)
      rem = 0.0;
   if (nf > 0.0)
   {
      ScanRun r;
      r.k1 = 0;
      r.k2 = g.N - 1;
      r.xs = g.x0;
      r.xe = g.x0 + g.N * g.p;
      r.w = nf;
      runs.push_back(r);
   }
   if (rem > 0.0)
   {
      double s0 = std::fmod(t0, T);
      if (s0 < 0.0)
         s0 += T;
      const double s1 = s0 + rem;
      if (s1 <= T)
         SegmentRuns(g, s0, s1, runs);
      else
      {
         SegmentRuns(g, s0, T, runs);
         SegmentRuns(g, 0.0, s1 - T, runs);
      }
   }
}

int ScanPeriodFrames(double E, double T)
{
   if (!(E > 0.0) || !(T > 0.0))
      return 1;
   const double r = E / T;
   for (int q = 1; q <= kScanMaxPeriod; ++q)
   {
      const double v = q * r;
      if (std::fabs(v - std::round(v)) <= 1e-9 * std::max(1.0, v))
         return q;
   }
   return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The spot.

void FocusedSpot::Make(double nm, double naEx, double nS, double zMaxUm)
{
   nm_ = nm;
   nS_ = nS;
   naEx_ = std::max(1e-3, std::min(naEx, 0.999 * nS));
   sinX_ = naEx_ / nS_;
   cosX_ = std::sqrt(std::max(0.0, 1.0 - sinX_ * sinX_));
   k_ = 2.0 * kPi * nS_ / (nm_ * 1e-3);
   const double lam = nm_ * 1e-3;
   const double dz0 = 0.1 * lam / (nS_ * (1.0 - cosX_));
   std::vector<double> zs;
   for (double z = 0.0; z < zMaxUm && static_cast<int>(zs.size()) < kMaxPlanes - 1; z += dz0 + kPlaneGrowth * z)
      zs.push_back(z);
   zs.push_back(std::max(zMaxUm, zs.empty() ? 0.0 : zs.back() + dz0));
   planes_.assign(zs.size(), Plane());
   ParallelFor(static_cast<unsigned>(zs.size()), [&](unsigned j) { MakePlane(planes_[j], zs[j]); });
   // Readouts: the in-focus peak, the lateral FWHM (the C table's projection would blur it: the radial half maximum,
   // found again by the Debye sum at a few radii), the axial FWHM from the planes' on-axis values.
   const Plane& q0 = planes_[0];
   peak_ = q0.Rc;   // (MakePlane leaves the on-axis value of plane 0 in Rc until here; see below)
   {
      // The on-axis value per plane (unit power): kept in Rc by MakePlane; collect, then reset Rc to the core width.
      std::vector<double> axial(planes_.size());
      for (size_t j = 0; j < planes_.size(); ++j)
         axial[j] = planes_[j].Rc;
      fwhmAx_ = 0.0;
      for (size_t j = 1; j < planes_.size(); ++j)
         if (axial[j] <= 0.5 * axial[0])
         {
            const double a = axial[j - 1], b = axial[j];
            const double t = a > b ? (a - 0.5 * axial[0]) / (a - b) : 0.0;
            fwhmAx_ = 2.0 * (planes_[j - 1].z + t * (planes_[j].z - planes_[j - 1].z));
            break;
         }
      for (Plane& q : planes_)
         q.Rc = 0.0;
   }
   // The lateral FWHM: the in-focus Airy-like radius at half the peak, by bisection on the Debye sum.
   auto debye0 = [&](double rho) {
      double re = 0.0;
      const int panels = static_cast<int>(std::ceil(k_ * rho * sinX_ / kPi)) + 2;
      const double th = std::asin(sinX_), dth = th / panels;
      for (int pn = 0; pn < panels; ++pn)
         for (int g = 0; g < 8; ++g)
         {
            const double t = (pn + 0.5 + 0.5 * kGx[g]) * dth;
            re += 0.5 * dth * kGw[g] * std::sqrt(std::cos(t)) * std::sin(t) * BesselJ0(k_ * rho * std::sin(t));
         }
      return re * re;
   };
   const double i0 = debye0(0.0);
   double lo = 0.0, hi = lam / naEx_;
   for (int it = 0; it < 50; ++it)
   {
      const double m = 0.5 * (lo + hi);
      (debye0(m) > 0.5 * i0 ? lo : hi) = m;
   }
   fwhmLat_ = lo + hi;
   // The core widths (the half-mass half width of each plane's projection), for the line-by-line sums.
   for (Plane& q : planes_)
   {
      const int n = 2 * q.M + 1;
      double w = q.R;
      for (int j = q.M; j < n; ++j)
         if (q.Q[static_cast<size_t>(j)] >= 0.75)
         {
            const double a = q.Q[static_cast<size_t>(j - 1)], b = q.Q[static_cast<size_t>(j)];
            const double t = b > a ? (0.75 - a) / (b - a) : 0.0;
            w = (j - 1 + t) * q.h - q.R;
            break;
         }
      q.Rc = std::max(0.0, w);
   }
}

void FocusedSpot::MakePlane(Plane& q, double z) const
{
   const double lam = nm_ * 1e-3;
   const double c99 = std::sqrt(cosX_ * cosX_ + (1.0 - kRayShare) * sinX_ * sinX_);
   const double tan99 = std::sqrt(std::max(0.0, 1.0 - c99 * c99)) / c99;
   const double R0 = z * tan99 + kTailLambdas * lam / naEx_;
   q.z = z;
   q.h = std::max(lam / (8.0 * naEx_), R0 / kTableHalf);
   q.M = std::max(4, static_cast<int>(std::ceil(R0 / q.h)));
   q.R = q.M * q.h;
   const double R = q.R;
   // The radial profile (unnormalized) on rho = i hr.
   const bool geometric = k_ * z * (1.0 - cosX_) > kDebyeAxialRad;
   const double hr = std::max(lam / (16.0 * naEx_), R / 1024.0);
   const int nr = static_cast<int>(std::ceil(R / hr)) + 2;
   std::vector<double> I(static_cast<size_t>(nr), 0.0);
   const double tanX = sinX_ / std::max(1e-12, cosX_);
   for (int i = 0; i < nr; ++i)
   {
      const double rho = i * hr;
      if (geometric)
      {
         const double tn = rho / z;
         if (tn <= tanX)
         {
            const double c2 = 1.0 / (1.0 + tn * tn);
            I[static_cast<size_t>(i)] = c2 * c2 / (z * z);
         }
         continue;
      }
      std::complex<double> a(0.0, 0.0);
      const int panels = static_cast<int>(std::ceil((k_ * z * (1.0 - cosX_) + k_ * rho * sinX_) / kPi)) + 2;
      const double th = std::asin(sinX_), dth = th / panels;
      for (int pn = 0; pn < panels; ++pn)
         for (int g = 0; g < 8; ++g)
         {
            const double t = (pn + 0.5 + 0.5 * kGx[g]) * dth;
            const double ct = std::cos(t), st = std::sin(t);
            const double m = 0.5 * dth * kGw[g] * std::sqrt(ct) * st * BesselJ0(k_ * rho * st);
            const double ph = k_ * z * ct;
            a += std::complex<double>(m * std::cos(ph), m * std::sin(ph));
         }
      I[static_cast<size_t>(i)] = std::norm(a);
   }
   auto Irad = [&](double rho) {
      if (rho >= R)
         return 0.0;
      const double f = rho / hr;
      const int i = static_cast<int>(f);
      if (i + 1 >= nr)
         return 0.0;
      const double t = f - i;
      return (1.0 - t) * I[static_cast<size_t>(i)] + t * I[static_cast<size_t>(i + 1)];
   };
   // The fine sweep: Cf[jf][i] = int_{-R}^{-R + i h} I dx at b = jf hf (trapezoid on kFine sub-steps).
   const int M = q.M, Mf = kFine * M, nA = 2 * M + 1, nAf = 2 * Mf + 1;
   const double hf = q.h / kFine;
   std::vector<double> Cf(static_cast<size_t>(Mf + 1) * nA, 0.0);
   for (int jf = 0; jf <= Mf; ++jf)
   {
      const double b = jf * hf;
      double cum = 0.0, prev = Irad(std::hypot(-R, b));
      double* row = Cf.data() + static_cast<size_t>(jf) * nA;
      row[0] = 0.0;
      for (int i = 1; i < nAf; ++i)
      {
         const double x = -R + i * hf;
         const double cur = Irad(std::hypot(x, b));
         cum += 0.5 * hf * (prev + cur);
         prev = cur;
         if (i % kFine == 0)
            row[i / kFine] = cum;
      }
   }
   // The plane's power (symmetric in b), and normalization to 1.
   double T = 0.5 * Cf[static_cast<size_t>(nA - 1)];
   for (int jf = 1; jf < Mf; ++jf)
      T += Cf[static_cast<size_t>(jf) * nA + nA - 1];
   T += 0.5 * Cf[static_cast<size_t>(Mf) * nA + nA - 1];
   T *= 2.0 * hf;
   const double inv = T > 0.0 ? 1.0 / T : 0.0;
   for (double& v : Cf)
      v *= inv;
   q.C.assign(static_cast<size_t>(M + 1) * nA, 0.0f);
   for (int j = 0; j <= M; ++j)
      for (int i = 0; i < nA; ++i)
         q.C[static_cast<size_t>(j) * nA + i] = static_cast<float>(Cf[static_cast<size_t>(j * kFine) * nA + i]);
   // D over signed b: the cumulative integral of C(a, |b|) from -R.
   q.D.assign(static_cast<size_t>(nA) * nA, 0.0f);
   for (int i = 0; i < nA; ++i)
   {
      double cum = 0.0, prev = Cf[static_cast<size_t>(Mf) * nA + i];
      for (int m = 1; m <= 2 * Mf; ++m)
      {
         const double cur = Cf[static_cast<size_t>(std::abs(m - Mf)) * nA + i];
         cum += 0.5 * hf * (prev + cur);
         prev = cur;
         if (m % kFine == 0)
            q.D[static_cast<size_t>(m / kFine) * nA + i] = static_cast<float>(cum);
      }
   }
   q.P.assign(static_cast<size_t>(M + 1), 0.0f);
   for (int j = 0; j <= M; ++j)
      q.P[static_cast<size_t>(j)] = q.C[static_cast<size_t>(j) * nA + nA - 1];
   q.Q.assign(static_cast<size_t>(nA), 0.0f);
   for (int j = 0; j < nA; ++j)
      q.Q[static_cast<size_t>(j)] = q.D[static_cast<size_t>(j) * nA + nA - 1];
   q.hr = hr;
   q.I.resize(I.size());
   for (size_t i = 0; i < I.size(); ++i)
      q.I[i] = static_cast<float>(I[i] * inv);
   q.Rc = I[0] * inv;   // (the on-axis value, unit power: Make reads it, then sets the core width)
}

size_t FocusedSpot::Bytes() const
{
   size_t b = 0;
   for (const Plane& q : planes_)
      b += (q.C.size() + q.D.size() + q.P.size() + q.Q.size() + q.I.size()) * sizeof(float);
   return b;
}

double FocusedSpot::PAt(const Plane& q, double b) const
{
   b = std::fabs(b);
   if (b >= q.R)
      return 0.0;
   return Tab1(q.P.data(), q.M + 1, b / q.h, true);
}

double FocusedSpot::QAt(const Plane& q, double b) const
{
   if (b <= -q.R)
      return 0.0;
   if (b >= q.R)
      return q.Q.back();
   return Tab1(q.Q.data(), 2 * q.M + 1, (b + q.R) / q.h, false);
}

double FocusedSpot::CAt(const Plane& q, double a, double b) const
{
   b = std::fabs(b);
   if (b >= q.R || a <= -q.R)
      return 0.0;
   if (a >= q.R)
      return PAt(q, b);
   return Tab2(q.C.data(), 2 * q.M + 1, q.M + 1, (a + q.R) / q.h, b / q.h, true);
}

double FocusedSpot::DAt(const Plane& q, double a, double b) const
{
   if (a <= -q.R || b <= -q.R)
      return 0.0;
   if (b > q.R)
      b = q.R;
   if (a >= q.R)
      return QAt(q, b);
   return Tab2(q.D.data(), 2 * q.M + 1, 2 * q.M + 1, (a + q.R) / q.h, (b + q.R) / q.h, false);
}

double FocusedSpot::RunSum(const Plane& q, const ScanProgram& g, const ScanRun& r, double x, double y,
                           bool interior) const
{
   const double as = x - r.xs, ae = x - r.xe, R = q.R, p = g.p;
   if (!interior && (as <= -R || ae >= R))
      return 0.0;
   const double yy = y - g.y0;
   const long lo = std::max<long>(r.k1, CeilL((yy - R) / p - 0.5));
   const long hi = std::min<long>(r.k2, FloorL((yy + R) / p - 0.5));
   if (lo > hi)
      return 0.0;
   auto f = [&](double b) { return interior ? PAt(q, b) : CAt(q, as, b) - CAt(q, ae, b); };
   auto F = [&](double b) { return interior ? QAt(q, b) : DAt(q, as, b) - DAt(q, ae, b); };
   // Lines near the point one by one (where the spot is sharp on the line spacing), the rest as their strips.
   const double Rc = q.Rc > 2.0 * p ? 0.0 : std::min(R, 6.0 * q.Rc + p);
   long c1 = hi + 1, c2 = hi;
   if (Rc > 0.0)
   {
      c1 = std::max(lo, CeilL((yy - Rc) / p - 0.5));
      c2 = std::min(hi, FloorL((yy + Rc) / p - 0.5));
   }
   double s = 0.0;
   auto far = [&](long ka, long kb) {
      if (ka > kb)
         return;
      const double bh = yy - ka * p, bl = yy - (kb + 1) * p;
      s += (F(bh) - F(bl)) / p;
   };
   if (c1 <= c2)
   {
      for (long k = c1; k <= c2; ++k)
         s += f(yy - (k + 0.5) * p);
      far(lo, c1 - 1);
      far(c2 + 1, hi);
   }
   else
      far(lo, hi);
   return r.w * s;
}

double FocusedSpot::PlaneSum(int j, const ScanProgram& g, const std::vector<ScanRun>& runs, double x, double y) const
{
   const Plane& q = planes_[static_cast<size_t>(j)];
   double s = 0.0;
   for (const ScanRun& r : runs)
      s += RunSum(q, g, r, x, y, x - r.xs >= q.R && x - r.xe <= -q.R);
   return s;
}

void FocusedSpot::Bracket(double zRel, int& j0, int& j1, double& t) const
{
   const double az = std::fabs(zRel);
   const int n = PlaneCount();
   if (az >= planes_.back().z || n == 1)
   {
      j0 = j1 = n - 1;
      t = 0.0;
      return;
   }
   int k = 0;
   while (k + 1 < n && planes_[static_cast<size_t>(k + 1)].z <= az)
      ++k;
   j0 = k;
   j1 = k + 1;
   t = (az - planes_[static_cast<size_t>(k)].z) / (planes_[static_cast<size_t>(k + 1)].z - planes_[static_cast<size_t>(k)].z);
}

double FocusedSpot::Sum(const ScanProgram& g, const std::vector<ScanRun>& runs, double x, double y, double zRel) const
{
   int j0, j1;
   double t;
   Bracket(zRel, j0, j1, t);
   const double a = PlaneSum(j0, g, runs, x, y);
   return j1 == j0 || !(t > 0.0) ? a : (1.0 - t) * a + t * PlaneSum(j1, g, runs, x, y);
}

void FocusedSpot::PlaneRow(int j, const ScanProgram& g, const std::vector<ScanRun>& runs, double y, double x0,
                           double dx, int nx, double scale, float* out) const
{
   const Plane& q = planes_[static_cast<size_t>(j)];
   for (const ScanRun& r : runs)
   {
      // The row's value where the run's whole sweep reaches (x far inside it), once.
      double inner = 0.0;
      bool haveInner = false;
      const int iLo = std::max(0, static_cast<int>(std::ceil((r.xs - q.R - x0) / dx)));
      const int iHi = std::min(nx - 1, static_cast<int>(std::floor((r.xe + q.R - x0) / dx)));
      for (int i = iLo; i <= iHi; ++i)
      {
         const double x = x0 + i * dx;
         const bool interior = x - r.xs >= q.R && x - r.xe <= -q.R;
         double v;
         if (interior)
         {
            if (!haveInner)
            {
               inner = RunSum(q, g, r, x, y, true);
               haveInner = true;
            }
            v = inner;
         }
         else
            v = RunSum(q, g, r, x, y, false);
         out[i] += static_cast<float>(scale * v);
      }
   }
}

// ---------------------------------------------------------------------------------------------------------------------
// A plane's light on a grid by FFT: the sweep rasterized (each line's swept length per cell along x, split linearly
// between the two rows about it), convolved with the spot plane's cell means (3 x 3 samples a cell, normalized to a
// sum of 1 / dx^2): the mean field's plane factors (a whole plane per frame; At stays exact point by point).

void FocusedSpot::Geometry(int j, const ScanProgram& g, double& x0, double& dx, int& n, int& m) const
{
   const Plane& q = planes_[static_cast<size_t>(j)];
   // Half the line spacing (the rows between the lines), or the table's step for a spot much wider than it.
   dx = q.h > g.p ? q.h : std::min(q.h, std::max(0.02, g.p / 2.0));
   // At most 1024 samples across the field and the reach on both sides.
   while ((g.N * g.p + 2.0 * q.R) / dx > 1024.0)
      dx *= 1.25;
   m = static_cast<int>(std::ceil(q.R / dx));
   n = static_cast<int>(std::ceil(g.N * g.p / dx)) + 2 * m + 1;
   x0 = g.x0 - m * dx;
}

const FocusedSpot::Kernel& FocusedSpot::KernelOf(int j, const ScanProgram& g) const
{
   std::lock_guard<std::mutex> lock(kernelMutex_);
   if (kernels_.size() != planes_.size())
      kernels_.resize(planes_.size());
   std::unique_ptr<Kernel>& K = kernels_[static_cast<size_t>(j)];
   if (K)
      return *K;
   K.reset(new Kernel());
   const Plane& q = planes_[static_cast<size_t>(j)];
   double x0, dx;
   int n, m;
   Geometry(j, g, x0, dx, n, m);
   const unsigned nf = RealFft2d::FastSize(static_cast<unsigned>(n + m), 2, true);
   K->fft = RealFft2d(nf, nf);
   std::vector<float> img(static_cast<size_t>(nf) * nf, 0.0f);
   auto Ir = [&](double rho) {
      const double f = rho / q.hr;
      const int i = static_cast<int>(f);
      if (i + 1 >= static_cast<int>(q.I.size()))
         return 0.0;
      const double t = f - i;
      return (1.0 - t) * q.I[static_cast<size_t>(i)] + t * q.I[static_cast<size_t>(i + 1)];
   };
   double sum = 0.0;
   for (int dy = -m; dy <= m; ++dy)
      for (int dxi = -m; dxi <= m; ++dxi)
      {
         double v = 0.0;
         for (int sy = -1; sy <= 1; ++sy)
            for (int sx = -1; sx <= 1; ++sx)
               v += Ir(std::hypot((dxi + sx / 3.0) * dx, (dy + sy / 3.0) * dx));
         v /= 9.0;
         const unsigned ix = static_cast<unsigned>((dxi + static_cast<int>(nf)) % static_cast<int>(nf));
         const unsigned iy = static_cast<unsigned>((dy + static_cast<int>(nf)) % static_cast<int>(nf));
         img[static_cast<size_t>(iy) * nf + ix] = static_cast<float>(v);
         sum += v;
      }
   const float sc = sum > 0.0 ? static_cast<float>(1.0 / (sum * dx * dx)) : 0.0f;
   for (float& v : img)
      v *= sc;
   K->spec.resize(K->fft.SpecSize());
   K->fft.Forward(img.data(), nf, nf, nf, K->spec.data());
   return *K;
}

void FocusedSpot::PlaneImage(int j, const ScanProgram& g, const std::vector<ScanRun>& runs, double norm,
                             std::vector<float>& out, double& x0, double& dx, int& n) const
{
   int m;
   Geometry(j, g, x0, dx, n, m);
   const Kernel& K = KernelOf(j, g);
   const unsigned nf = K.fft.Nx();
   std::vector<float> path(static_cast<size_t>(nf) * nf, 0.0f);
   std::vector<float> wx(static_cast<size_t>(n));
   for (const ScanRun& r : runs)
   {
      // The run's swept length per column (the same for each of its lines).
      std::fill(wx.begin(), wx.end(), 0.0f);
      const int i0 = std::max(0, static_cast<int>(std::floor((r.xs - x0) / dx + 0.5)));
      const int i1 = std::min(n - 1, static_cast<int>(std::floor((r.xe - x0) / dx + 0.5)));
      for (int i = i0; i <= i1; ++i)
      {
         const double a = std::max(r.xs, x0 + (i - 0.5) * dx), b = std::min(r.xe, x0 + (i + 0.5) * dx);
         if (b > a)
            wx[static_cast<size_t>(i)] = static_cast<float>(r.w * (b - a));
      }
      for (int k = r.k1; k <= r.k2; ++k)
      {
         const double fy = (g.LineY(k) - x0) / dx;
         const int jy = static_cast<int>(std::floor(fy));
         const double t = fy - jy;
         for (int b = 0; b < 2; ++b)
         {
            const int row = jy + b;
            const float wy = static_cast<float>(b ? t : 1.0 - t);
            if (row < 0 || row >= n || !(wy > 0.0f))
               continue;
            float* pr = path.data() + static_cast<size_t>(row) * nf;
            for (int i = i0; i <= i1; ++i)
               pr[i] += wy * wx[static_cast<size_t>(i)];
         }
      }
   }
   std::vector<cfloat> spec(K.fft.SpecSize());
   K.fft.Forward(path.data(), nf, nf, nf, spec.data(), true);
   for (size_t i = 0; i < spec.size(); ++i)
      spec[i] *= K.spec[i];
   out.assign(static_cast<size_t>(n) * n, 0.0f);
   K.fft.Inverse(spec.data(), out.data(), 0, static_cast<unsigned>(n), 0, static_cast<unsigned>(n), static_cast<size_t>(n));
   const float sc = static_cast<float>(norm);
   for (float& v : out)
      v = std::max(0.0f, v * sc);
}

std::shared_ptr<const ScanPlaneImage> ScanLight::PlaneImageOf(int l, int j, const std::vector<ScanRun>& runs,
                                                              double norm) const
{
   uint64_t h = BfpHashBytes(&l, sizeof l);
   h = BfpHashBytes(&j, sizeof j, h);
   h = BfpHashBytes(&norm, sizeof norm, h);
   for (const ScanRun& r : runs)
   {
      const double v[5] = { static_cast<double>(r.k1), static_cast<double>(r.k2), r.xs, r.xe, r.w };
      h = BfpHashBytes(v, sizeof v, h);
   }
   {
      std::lock_guard<std::mutex> lock(imageMutex_);
      for (auto it = images_.begin(); it != images_.end(); ++it)
         if (it->first == h)
         {
            auto r = it->second;
            images_.splice(images_.begin(), images_, it);
            return r;
         }
   }
   auto im = std::make_shared<ScanPlaneImage>();
   spots_[static_cast<size_t>(l)].PlaneImage(j, prog_, runs, norm, im->v, im->x0, im->dx, im->n);
   std::lock_guard<std::mutex> lock(imageMutex_);
   images_.emplace_front(h, im);
   size_t bytes = 0;
   for (const auto& e : images_)
      bytes += e.second->v.size() * sizeof(float);
   while (images_.size() > 1 && bytes > kScanImageBytes)
   {
      bytes -= images_.back().second->v.size() * sizeof(float);
      images_.pop_back();
   }
   return im;
}

// ---------------------------------------------------------------------------------------------------------------------
// The scan light.

void ScanLight::FrameRuns(long long frame, double expSec, std::vector<ScanRun>& runs, double& norm) const
{
   const double E = std::max(1e-9, expSec);
   ScanRunsOf(prog_, static_cast<double>(frame) * E, E, runs);
   norm = prog_.K / E;
}

void ScanLight::MeanRuns(std::vector<ScanRun>& runs, double& norm) const
{
   runs.assign(1, ScanRun());
   runs[0].k1 = 0;
   runs[0].k2 = prog_.N - 1;
   runs[0].xs = prog_.x0;
   runs[0].xe = prog_.x0 + prog_.N * prog_.p;
   runs[0].w = 1.0;
   norm = prog_.K / prog_.scanSec;
}

double ScanLight::PeakPerSetting(int l) const
{
   const double A = (prog_.N * prog_.p) * (prog_.N * prog_.p);
   return A / prog_.duty * spots_[static_cast<size_t>(l)].PeakPerUm2();
}

size_t ScanLight::Bytes() const
{
   size_t b = 0;
   for (const FocusedSpot& s : spots_)
      b += s.Bytes();
   return b;
}

struct ScanLightBuilder
{
   static std::shared_ptr<const BfpLight> Build(const ScanLightParams& P, uint64_t hash)
   {
      const auto t0 = TimingClock::now();
      auto S = std::make_shared<ScanLight>();
      S->params_ = P;
      ScanProgram& g = S->prog_;
      g.p = std::max(1e-3, P.pixelUm);
      g.N = std::max(1, static_cast<int>(std::ceil(std::max(g.p, P.fieldUm) / g.p - 1e-9)));
      g.tau = std::max(1e-9, P.dwellSec);
      g.fb = std::max(0.0, P.flybackSec);
      g.x0 = g.y0 = -0.5 * g.N * g.p;
      g.sweepSec = g.N * g.tau;
      g.lineSec = g.sweepSec + g.fb;
      g.scanSec = g.N * g.lineSec;
      g.v = g.p / g.tau;
      g.duty = g.sweepSec / g.lineSec;
      g.K = (g.N * g.p) * (g.N * g.p) / (g.duty * g.v);
      std::vector<double> lines = P.lines;
      if (lines.empty())
         lines.push_back(P.lambdaRefNm);
      const double naEx = std::max(1e-3, P.fill) * P.na;
      S->spots_.resize(lines.size());
      for (size_t l = 0; l < lines.size(); ++l)
         S->spots_[l].Make(lines[l], naEx, P.nS, std::max(1.0, P.zSampleUm));

      auto L = std::make_shared<BfpLight>();
      BfpLightParams& bp = L->params_;
      bp.text = "# CSLM";
      bp.lines = lines;
      bp.lambdaRefNm = P.lambdaRefNm;
      bp.na = P.na;
      bp.nS = P.nS;
      bp.fovSpanUm = P.fieldUm;
      bp.zSampleUm = P.zSampleUm;
      L->hash_ = hash;
      L->pivotDependent_ = false;
      L->slots_ = 1;
      L->period_ = 1;
      L->uOuter_ = std::min(naEx, 0.999 * P.nS) / P.nS;
      L->evanescent_ = false;
      L->evanescentShare_ = 0.0;
      L->propagating_ = true;
      L->tanS_ = 0.0;
      L->decayPerNm_ = 0.0;
      std::vector<ScanRun> runs;
      double norm = 1.0;
      S->MeanRuns(runs, norm);
      double reach = 0.0, halfC = 0.0, fwhm = 1e9;
      L->lines_.resize(lines.size());
      for (size_t l = 0; l < lines.size(); ++l)
      {
         const FocusedSpot& sp = S->spots_[l];
         fwhm = std::min(fwhm, sp.FwhmLateralUm());
         // The mean's planes: at most 13 a side of the spot's (both ends kept), mirrored about the focus.
         const int np = sp.PlaneCount();
         std::vector<int> pick;
         const int stride = std::max(1, (np + 11) / 12);
         for (int j = 0; j < np; j += stride)
            pick.push_back(j);
         if (pick.back() != np - 1)
            pick.push_back(np - 1);
         BfpVolume V;
         V.relative = true;
         V.extendTop = V.extendBottom = true;
         std::vector<int> planeOf;   // per volume plane, its spot plane
         for (size_t q = pick.size(); q-- > 1;)
         {
            V.z.push_back(-sp.PlaneZ(pick[q]));
            planeOf.push_back(pick[q]);
         }
         for (int j : pick)
         {
            V.z.push_back(sp.PlaneZ(j));
            planeOf.push_back(j);
         }
         double rMax = 0.0;
         for (int j : pick)
            rMax = std::max(rMax, sp.PlaneReachUm(j));
         const double halfField = 0.5 * g.N * g.p;
         // The tile over the field (its edges in focus: a third of the spot), the window beyond it to the reach.
         double tdx = std::max(0.03, std::min(0.25, sp.FwhmLateralUm() / 2.5));
         const double halfT = halfField + 2.0;
         while ((2.0 * halfT / tdx + 1) * (2.0 * halfT / tdx + 1) * V.z.size() > 6e6)
            tdx *= 1.25;
         V.tn = 2 * static_cast<int>(std::ceil(halfT / tdx)) + 1;
         V.tdx = tdx;
         V.tx0 = -(V.tn - 1) / 2 * tdx;
         halfC = std::max(halfC, halfField + rMax);
         const double hc = halfField + rMax;
         V.dx = std::max(2.0 * tdx, 2.0 * hc / 256.0);
         V.nx = V.ny = 2 * static_cast<int>(std::ceil(hc / V.dx)) + 1;
         V.x0 = V.y0 = -(V.nx - 1) / 2 * V.dx;
         const int nz = V.Nz();
         V.t.assign(static_cast<size_t>(nz) * V.tn * V.tn, 0.0f);
         V.v.assign(static_cast<size_t>(nz) * V.nx * V.ny, 0.0f);
         // Each spot plane once (the volume's +z and -z share it).
         const int nPick = static_cast<int>(pick.size());
         ParallelFor(static_cast<unsigned>(nPick * (V.tn + V.ny)), [&](unsigned idx) {
            const int qk = static_cast<int>(idx) / (V.tn + V.ny), row = static_cast<int>(idx) % (V.tn + V.ny);
            const int j = pick[static_cast<size_t>(qk)];
            const int kz = nPick - 1 + qk;   // the +z plane's index in V.z
            if (row < V.tn)
               sp.PlaneRow(j, g, runs, V.tx0 + row * tdx, V.tx0, tdx, V.tn, norm,
                           V.t.data() + (static_cast<size_t>(kz) * V.tn + row) * V.tn);
            else
            {
               const int r = row - V.tn;
               sp.PlaneRow(j, g, runs, V.y0 + r * V.dx, V.x0, V.dx, V.nx, norm,
                           V.v.data() + (static_cast<size_t>(kz) * V.ny + r) * V.nx);
            }
         });
         for (int qk = 1; qk < nPick; ++qk)
         {
            const int src = nPick - 1 + qk, dst = nPick - 1 - qk;
            std::copy(V.t.begin() + static_cast<long>(src) * V.tn * V.tn, V.t.begin() + static_cast<long>(src + 1) * V.tn * V.tn,
                      V.t.begin() + static_cast<long>(dst) * V.tn * V.tn);
            std::copy(V.v.begin() + static_cast<long>(src) * V.nx * V.ny, V.v.begin() + static_cast<long>(src + 1) * V.nx * V.ny,
                      V.v.begin() + static_cast<long>(dst) * V.nx * V.ny);
         }
         // The reach: the farthest window sample at 1e-4 of the interior or more.
         for (int k = 0; k < nz; ++k)
            for (int j = 0; j < V.ny; ++j)
               for (int i = 0; i < V.nx; ++i)
                  if (V.v[(static_cast<size_t>(k) * V.ny + j) * V.nx + i] >= 1e-4f)
                     reach = std::max(reach, std::hypot(V.x0 + i * V.dx, V.y0 + j * V.dx));
         L->lines_[l].nm = lines[l];
         L->lines_[l].mean.parts.push_back(std::move(V));
      }
      L->halfWindowUm_ = halfC;
      L->reachUm_ = reach > 0.0 ? reach : halfC;
      // The mean's sharpest bend: its in-focus edge, an error function of the spot's sigma (max |E''| = 0.24 / s^2).
      const double sig = std::max(0.02, fwhm / 2.3548);
      L->curvature_ = 0.25 / (sig * sig);
      L->scan_ = S;
      L->computeSec_ = TimingSince(t0);
      char b[320];
      std::snprintf(b, sizeof b,
                    "CSLM: %d lines of %.0f nm, line %.3f ms, scan %.4g s; spot FWHM %.0f nm lateral, %.0f nm axial "
                    "(NA %.3g); peak %.4g x the field mean; %.2f s, %.1f MB",
                    g.N, g.p * 1e3, g.lineSec * 1e3, g.scanSec, S->spots_[0].FwhmLateralUm() * 1e3,
                    S->spots_[0].FwhmAxialUm() * 1e3, std::min(naEx, 0.999 * P.nS), S->PeakPerSetting(0),
                    L->computeSec_, (L->Bytes() + S->Bytes()) / 1048576.0);
      L->info_ = b;
      return L;
   }
};

namespace {
std::mutex& ScanMemoMutex()
{
   static std::mutex m;
   return m;
}
std::list<std::pair<uint64_t, std::shared_ptr<const BfpLight>>>& ScanMemo()
{
   static std::list<std::pair<uint64_t, std::shared_ptr<const BfpLight>>> l;
   return l;
}
constexpr size_t kScanMemo = 4;

uint64_t ScanHash(const ScanLightParams& p)
{
   const char tag[] = "CSLM-1";
   uint64_t h = BfpHashBytes(tag, sizeof tag);
   auto mix = [&](double v) { h = BfpHashBytes(&v, sizeof v, h); };
   mix(static_cast<double>(p.lines.size()));
   for (double nm : p.lines)
      mix(nm);
   for (double v : { p.lambdaRefNm, p.na, p.nS, p.fieldUm, p.pixelUm, p.dwellSec, p.flybackSec, p.fill, p.zSampleUm })
      mix(v);
   return h;
}
} // namespace

std::shared_ptr<const BfpLight> MakeScanLight(const ScanLightParams& p, std::string& err)
{
   err.clear();
   if (!(p.pixelUm > 0.0) || !(p.dwellSec > 0.0) || !(p.fill > 0.0) || !(p.na > 0.0))
   {
      err = "CSLM: the scan pixel, dwell, fill and NA must be positive";
      return nullptr;
   }
   const uint64_t h = ScanHash(p);
   {
      std::lock_guard<std::mutex> g(ScanMemoMutex());
      for (auto it = ScanMemo().begin(); it != ScanMemo().end(); ++it)
         if (it->first == h)
         {
            auto r = it->second;
            ScanMemo().splice(ScanMemo().begin(), ScanMemo(), it);
            return r;
         }
   }
   auto L = ScanLightBuilder::Build(p, h);
   TimingLog("fl.cslm-light", L->ComputeSec(), L->Info().c_str());
   std::lock_guard<std::mutex> g(ScanMemoMutex());
   ScanMemo().emplace_front(h, L);
   while (ScanMemo().size() > kScanMemo)
      ScanMemo().pop_back();
   return L;
}

} // namespace sim
