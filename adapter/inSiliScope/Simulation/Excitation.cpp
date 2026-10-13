///////////////////////////////////////////////////////////////////////////////
// FILE:          Excitation.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Excitation.h; the same arithmetic as excitation.js, in its
//                operand order.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Excitation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim {

namespace {
constexpr double kUGap = 1e-3;
constexpr double kFlatTopEdge = 0.15;   // the logistic edge width, x the disc radius (SMLMBackground.cpp)
constexpr double kPi = 3.14159265358979323846;
} // namespace

const std::vector<std::string>& IlluminationProfileNames()
{
   static const std::vector<std::string> v = { "Flat", "Gaussian", "FlatTop" };
   return v;
}

const std::vector<std::string>& IlluminationRegimeNames()
{
   static const std::vector<std::string> v = { "Epi", "HILO", "TIRF", "Clipped" };
   return v;
}

void Excitation::Make(const ExcitationSpec& s)
{
   profile_ = s.profile;
   fwhmUm_ = std::max(1e-6, (s.fwhmPct / 100.0) * s.fovWidthUm);
   const double nI = s.nImm, nS = s.nMed;
   nS_ = nS;
   zp_ = s.pivotUm;
   double u = std::max(0.0, s.bfp);
   uMax_ = s.na / nS;
   clipped_ = u > uMax_;
   if (std::fabs(u - 1.0) < kUGap)
      u = u < 1.0 ? 1.0 - kUGap : 1.0 + kUGap;
   u_ = u;
   tirf_ = !clipped_ && u > 1.0;
   regime_ = clipped_ ? REGIME_CLIPPED : (tirf_ ? REGIME_TIRF : (u > 0.0 ? REGIME_HILO : REGIME_EPI));
   const double sinI = u * nS / nI;
   cosI_ = std::sqrt(std::max(0.0, 1.0 - sinI * sinI));
   const double n = nS / nI;
   F_ = 0.0;
   tanS_ = 0.0;
   if (!clipped_)
   {
      if (tirf_)
      {
         const double c2 = cosI_ * cosI_, s2 = sinI * sinI;
         const double Is = 4.0 * c2 / (1.0 - n * n);
         const double Ip = 4.0 * c2 * (2.0 * s2 - n * n) / (n * n * n * n * c2 + s2 - n * n);
         const double t0 = 2.0 * nI / (nI + nS);
         F_ = (Is + Ip) / 2.0 / (t0 * t0);
      }
      else
      {
         const double sinS = u, cosS = std::sqrt(1.0 - sinS * sinS);
         tanS_ = sinS / cosS;
         const double ts = 2.0 * nI * cosI_ / (nI * cosI_ + nS * cosS), tp = 2.0 * nI * cosI_ / (nS * cosI_ + nI * cosS);
         const double t0 = 2.0 * nI / (nI + nS);
         F_ = (ts * ts + tp * tp) / 2.0 / (t0 * t0);
      }
   }
   sig_ = fwhmUm_ / 2.3548;
   r0_ = fwhmUm_ / 2.0;
   edge_ = std::max(1e-6, kFlatTopEdge * r0_);
   flatTop0_ = 1.0 / (1.0 + std::exp(-r0_ / edge_));
   reachUm_ = profile_ == ILLUM_FLAT ? std::numeric_limits<double>::infinity()
                                     : (profile_ == ILLUM_GAUSSIAN ? sig_ * 7.5 : r0_ + 28.0 * edge_);
   uniform_ = profile_ == ILLUM_FLAT && u == 0.0;
}

double Excitation::ProfileAt(double xp, double y) const
{
   if (profile_ == ILLUM_FLAT)
      return 1.0;
   const double r2 = xp * xp + y * y;
   if (profile_ == ILLUM_GAUSSIAN)
      return std::exp(-r2 / (2.0 * sig_ * sig_));
   return 1.0 / (1.0 + std::exp((std::sqrt(r2) - r0_) / edge_)) / flatTop0_;
}

double Excitation::DecayUm(double nm) const
{
   return 1e-3 * nm / (4.0 * kPi * nS_ * std::sqrt(u_ * u_ - 1.0));
}

double Excitation::Lateral(double x, double y, double z) const
{
   return ProfileAt((x - (tirf_ ? 0.0 : z - zp_) * tanS_) * cosI_, y);
}

double Excitation::Depth(double z, const ExcitationShares& shares) const
{
   if (!tirf_)
      return 1.0;
   double s = 0.0;
   for (const ExcitationLine& l : shares)
      s += l.share * std::exp(-std::max(0.0, z) / DecayUm(l.nm));
   return s;
}

double Excitation::DepthMean(double z0, double z1, const ExcitationShares& shares) const
{
   if (!tirf_)
      return 1.0;
   const double a = std::max(0.0, z0), b = std::max(a, z1);
   if (!(b > a))
      return Depth(a, shares);
   double s = 0.0;
   for (const ExcitationLine& l : shares)
   {
      const double d = DecayUm(l.nm);
      s += l.share * d * (std::exp(-a / d) - std::exp(-b / d));
   }
   return s / (b - a);
}

double Excitation::At(double x, double y, double z, const ExcitationShares& shares) const
{
   return clipped_ ? 0.0 : F_ * Lateral(x, y, z) * Depth(z, shares);
}

double Excitation::Column(double x, double y, double H, const ExcitationShares& shares) const
{
   if (clipped_ || !(H > 0.0))
      return 0.0;
   if (tirf_)
   {
      double s = 0.0;
      for (const ExcitationLine& l : shares)
      {
         const double d = DecayUm(l.nm);
         s += l.share * d * (1.0 - std::exp(-H / d));
      }
      return F_ * Lateral(x, y, 0.0) * s;
   }
   if (tanS_ == 0.0 || profile_ == ILLUM_FLAT)
      return F_ * Lateral(x, y, 0.0) * H;
   const double r = reachUm_ / cosI_;
   const double za = std::max(0.0, zp_ + (x - r) / tanS_), zb = std::min(H, zp_ + (x + r) / tanS_);
   if (!(zb > za))
      return 0.0;
   const int n = 256;
   const double dz = (zb - za) / n;
   double s = 0.0;
   for (int i = 0; i < n; i++)
      s += Lateral(x, y, za + (i + 0.5) * dz);
   return F_ * s * dz;
}

double Excitation::DepthNm(double lambdaNm) const
{
   return tirf_ ? DecayUm(lambdaNm) * 1000.0
                : (!clipped_ && tanS_ > 0.0 && profile_ != ILLUM_FLAT ? fwhmUm_ / (tanS_ * cosI_) * 1000.0 : -1.0);
}

std::vector<double> DoseNodes(double aMax, double dMax)
{
   std::vector<double> out;
   if (!(aMax > 0))
      return out;
   if (!(dMax > kDoseStep))
   {
      out.push_back(aMax);
      return out;
   }
   const int K = std::min(kDoseLevels, static_cast<int>(std::ceil(std::min(dMax, kDoseCut) / kDoseStep)));
   const double aCut = dMax > kDoseCut ? aMax * kDoseCut / dMax : aMax;
   for (int k = 1; k <= K; k++)
      out.push_back(aCut * k / K);
   if (aCut < aMax)
      out.push_back(aMax);
   return out;
}

double HatWeight(const std::vector<double>& nodes, size_t k, double a)
{
   const double hi = nodes[k], lo = k > 0 ? nodes[k - 1] : 0.0;
   if (a >= lo && a <= hi)
      return hi > lo ? (a - lo) / (hi - lo) : 1.0;
   if (k + 1 < nodes.size())
   {
      const double nx = nodes[k + 1];
      if (a > hi && a < nx)
         return (nx - a) / (nx - hi);
   }
   return 0.0;
}

const std::vector<double>& HistoryBandEdges()
{
   static const std::vector<double> e = { 0.1, 0.2, 0.4, 0.7, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.5 };
   return e;
}

void HistoryBandRange(int band, double& z0Um, double& z1Um)
{
   const std::vector<double>& e = HistoryBandEdges();
   z0Um = band <= 0 ? 0.0 : e[static_cast<size_t>(band - 1)];
   z1Um = band >= kHistoryBands - 1 ? 8.0 : e[static_cast<size_t>(band)];
}

HistoryLightField::HistoryLightField(const Excitation& ex, const std::vector<ExcitationLine>& sharesIn)
   : ex_(ex), amp_(kHistoryBands), zMid_(kHistoryBands),
     sheet_(ex.Regime() == REGIME_HILO)   // the footprint moves with the height
{
   const ExcitationShares shares = sharesIn.empty() ? ExcitationShares{ { 640.0, 1.0 } } : sharesIn;
   // Per band: F x its mean depth factor in 1/16 steps (0 when clipped), and the height the footprint is read at.
   for (int b = 0; b < kHistoryBands; ++b)
   {
      double z0, z1;
      HistoryBandRange(b, z0, z1);
      amp_[static_cast<size_t>(b)] = ex.Clipped() ? 0.0 : std::round(16.0 * ex.F() * ex.DepthMean(z0, z1, shares)) / 16.0;
      zMid_[static_cast<size_t>(b)] = (z0 + z1) / 2;
   }
}

double HistoryLightField::Band(double dxUm, double dyUm, int b) const
{
   const size_t k = static_cast<size_t>(std::min(kHistoryBands - 1, std::max(0, b)));
   return ex_.Lateral(dxUm, dyUm, sheet_ ? zMid_[k] : 0.0) >= 0.5 ? amp_[k] : 0.0;
}

void HistoryLightField::Fill(double dxUm, double dyUm, float* w) const
{
   const bool lit0 = !sheet_ && ex_.Lateral(dxUm, dyUm, 0.0) >= 0.5;
   for (int b = 0; b < kHistoryBands; ++b)
      w[b] = static_cast<float>(sheet_ ? Band(dxUm, dyUm, b) : lit0 ? amp_[static_cast<size_t>(b)] : 0.0);
}

int HistoryLightField::BandOf(double zUm)
{
   const std::vector<double>& e = HistoryBandEdges();
   return static_cast<int>(std::upper_bound(e.begin(), e.end(), zUm) - e.begin());
}

double HistoryLightField::TileCentre(double vUm)
{
   const double T = kHistoryTileUm;
   return (std::floor(vUm / T) + 0.5) * T;
}

} // namespace sim
