///////////////////////////////////////////////////////////////////////////////
// FILE:          Drift.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Drift.h: the drift path (pure, no FFT: the webSMLM block links it alone);
//                the image shifts (ApplyShiftRamp, ShiftedCamera) are in DriftShift.cpp.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Drift.h"

#include "SMLMCounterRng.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

uint32_t DriftSeed(long randomSeed)
{
   return static_cast<uint32_t>(static_cast<uint64_t>(randomSeed) ^ 0x44524654ULL); // "DRFT"
}

DriftNm DriftStep(uint32_t driftSeed, long f, double frameSec, const DriftSettings& s)
{
   // One counter stream per frame: x, y, z in that order (statements, not
   // one expression: the draw order is fixed).
   CounterRng u(driftSeed, static_cast<uint32_t>(f));
   u.Pixel(0);
   const double gx = CounterGauss(u);
   const double gy = CounterGauss(u);
   const double gz = CounterGauss(u);
   const double r = std::sqrt(std::max(0.0, frameSec));
   DriftNm d;
   d.x = s.xyNmPerSqrtSec * r * gx;
   d.y = s.xyNmPerSqrtSec * r * gy;
   d.z = s.zNmPerSqrtSec * r * gz;
   return d;
}

double DriftSwing(double x)
{
   const double z = std::fabs(x) / std::sqrt(2.0);
   const double t = 1.0 / (1.0 + 0.3275911 * z);
   const double poly = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t;
   const double e = 1.0 - poly * std::exp(-z * z);
   return x < 0.0 ? -e : e;
}

DriftWalker::DriftWalker(long randomSeed, const DriftSettings& s, long f0)
   : seed_(DriftSeed(randomSeed)), s_(s), f_(f0)
{
   // theta0 and the z direction: once per seed (frame 0xFFFFFFFF, pixel 2), whatever f0.
   CounterRng a(seed_, 0xFFFFFFFFu);
   a.Pixel(2);
   theta0_ = 2.0 * kPi * a.Uniform();
   zUp_ = a.Uniform() < 0.5;
   // The wanders start from their stationary distribution (unit variance).
   CounterRng u(seed_, static_cast<uint32_t>(f0));
   u.Pixel(1);
   phi_ = CounterGauss(u);
   sxy_ = CounterGauss(u);
   sz_ = CounterGauss(u);
   CounterRng v(seed_, static_cast<uint32_t>(f0));
   v.Pixel(3);
   psi_ = CounterGauss(v);
}

const DriftNm& DriftWalker::Step(double frameSec)
{
   const double dt = std::max(0.0, frameSec);
   // Velocity at the frame's start.
   const double theta = (s_.xyAngleDeg >= 0.0 ? s_.xyAngleDeg * kPi / 180.0 : theta0_) +
                        s_.angleWanderDeg * kPi / 180.0 * DriftSwing(phi_);
   const double w = s_.speedWanderPct / 100.0;
   const double vxy = s_.xySpeedNmPerSec * std::max(0.0, 1.0 + w * sxy_);
   const double up = s_.zDirection > 0 ? 1.0 : s_.zDirection < 0 ? -1.0 : (zUp_ ? 1.0 : -1.0);
   const double vz = up * s_.zSpeedNmPerSec * std::max(0.0, 1.0 + w * sz_) *
                     std::cos(s_.zAngleWanderDeg * kPi / 180.0 * DriftSwing(psi_));
   const double vx = vxy * std::cos(theta), vy = vxy * std::sin(theta);
   ++f_;
   const DriftNm st = DriftStep(seed_, f_, frameSec, s_);
   d_.x = d_.x + vx * dt + st.x;
   d_.y = d_.y + vy * dt + st.y;
   d_.z = d_.z + vz * dt + st.z;
   // The wanders (unit-variance Ornstein-Uhlenbeck, exact per step).
   CounterRng u(seed_, static_cast<uint32_t>(f_));
   u.Pixel(1);
   const double gp = CounterGauss(u);
   const double gs = CounterGauss(u);
   const double gz = CounterGauss(u);
   const double a = s_.wanderTimeSec > 0.0 ? std::exp(-dt / s_.wanderTimeSec) : 0.0;
   const double b = std::sqrt(std::max(0.0, 1.0 - a * a));
   phi_ = a * phi_ + b * gp;
   sxy_ = a * sxy_ + b * gs;
   sz_ = a * sz_ + b * gz;
   CounterRng v(seed_, static_cast<uint32_t>(f_));
   v.Pixel(3);
   psi_ = a * psi_ + b * CounterGauss(v);
   return d_;
}

std::vector<DriftNm> DriftTrajectory(long randomSeed, long frames, double frameSec, const DriftSettings& s)
{
   std::vector<DriftNm> t(static_cast<size_t>(std::max(0L, frames)));
   if (!s.On())
      return t;
   DriftWalker walker(randomSeed, s);
   for (long f = 1; f < frames; ++f)
      t[static_cast<size_t>(f)] = walker.Step(frameSec);
   return t;
}

double DriftBounds::MaxXyNm() const
{
   return std::max(std::max(-xLo, xHi), std::max(-yLo, yHi));
}

DriftBounds DriftRange(const std::vector<DriftNm>& t)
{
   DriftBounds b;
   for (const DriftNm& d : t)
   {
      b.xLo = std::min(b.xLo, d.x);
      b.xHi = std::max(b.xHi, d.x);
      b.yLo = std::min(b.yLo, d.y);
      b.yHi = std::max(b.yHi, d.y);
      b.zLo = std::min(b.zLo, d.z);
      b.zHi = std::max(b.zHi, d.z);
   }
   return b;
}

void DriftWidenZCull(const DriftBounds& b, double& centreUm, double& halfUm)
{
   centreUm -= (b.zLo + b.zHi) / 2000.0;
   halfUm += (b.zHi - b.zLo) / 2000.0;
}

DriftFocusGrid DriftFocusGrid::For(const DriftBounds& b)
{
   DriftFocusGrid g;
   const long lo = static_cast<long>(std::floor(b.zLo / kDriftFocusStepNm));
   const long hi = static_cast<long>(std::ceil(b.zHi / kDriftFocusStepNm));
   g.k0 = lo;
   g.n = static_cast<int>(hi - lo + 1);
   return g;
}

void DriftFocusGrid::Weights(double dzNm, int& k, double& w) const
{
   if (n <= 1)
   {
      k = 0;
      w = 0.0;
      return;
   }
   const double t = dzNm / kDriftFocusStepNm - static_cast<double>(k0);
   k = std::min(n - 2, std::max(0, static_cast<int>(std::floor(t))));
   w = std::min(1.0, std::max(0.0, t - k));
}

} // namespace sim
