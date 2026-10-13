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
#include "Parallel.h"

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
   nI_ = nI;
   na_ = s.na;
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

namespace {
// Gauss-Legendre, 16 points on [-1, 1]: the positive nodes and their weights (= excitation.js GL16_X / GL16_W).
const double kGl16X[8] = { 0.09501250983763744, 0.2816035507792589, 0.45801677765722737, 0.6178762444026438,
                           0.755404408355003,   0.8656312023878318, 0.9445750230732326,  0.9894009349916499 };
const double kGl16W[8] = { 0.18945061045506864, 0.18260341504492364, 0.16915651939500265, 0.1495959888165767,
                           0.12462897125553407, 0.0951585116824926,  0.062253523938647456, 0.027152459411754176 };

// The ring directions: (cos, sin) of 2 pi (k + 0.5) / nd, interleaved.
std::vector<double> RingDirs(int nd)
{
   std::vector<double> ds;
   for (int k = 0; k < nd; k++)
   {
      const double a = 2.0 * kPi * (k + 0.5) / nd;
      ds.push_back(std::cos(a));
      ds.push_back(std::sin(a));
   }
   return ds;
}
} // namespace

CollectionRings CollectionRingsFor(double nS, double nI, double na, int n, bool fresnel)
{
   const double cMin = na >= nS ? 0.0 : std::sqrt(1.0 - (na / nS) * (na / nS));
   const int kSteps = 4096;
   const double dc = (1.0 - cMin) / kSteps;
   auto T = [&](double c) {
      if (!fresnel)
         return 1.0;
      const double s = std::sqrt(1.0 - c * c), sg = nS * s / nI, cg = std::sqrt(1.0 - sg * sg);
      const double ts = 2.0 * nS * c / (nS * c + nI * cg), tp = 2.0 * nS * c / (nI * c + nS * cg);
      return (nI * cg) / (nS * c) * (ts * ts + tp * tp) / 2.0;
   };
   std::vector<double> cum(kSteps + 1, 0.0);
   for (int k = 0; k < kSteps; k++)
      cum[k + 1] = cum[k] + T(1.0 - (k + 0.5) * dc) * dc;
   std::vector<double> qs;
   CollectionRings r;
   if (n == 0)
   {
      for (int j = 7; j >= 0; j--)
      {
         qs.push_back(0.5 - 0.5 * kGl16X[j]);
         r.weights.push_back(0.5 * kGl16W[j]);
      }
      for (int j = 0; j < 8; j++)
      {
         qs.push_back(0.5 + 0.5 * kGl16X[j]);
         r.weights.push_back(0.5 * kGl16W[j]);
      }
   }
   else
      for (int j = 0; j < n; j++)
      {
         qs.push_back((j + 0.5) / n);
         r.weights.push_back(1.0 / n);
      }
   int k = 0;
   for (double qq : qs)
   {
      const double q = qq * cum[kSteps];
      while (k < kSteps - 1 && cum[k + 1] < q)
         k++;
      const double c = 1.0 - (k + (q - cum[k]) / (cum[k + 1] - cum[k])) * dc;
      r.tans.push_back(std::sqrt(1.0 - c * c) / c);
   }
   return r;
}

std::vector<Excitation::Interval> Excitation::Intervals(double Hc, const std::vector<ImagerSource>& imagers,
                                                         double ratio, int nTirf) const
{
   std::vector<Interval> out;
   auto add = [&](double a, double b) {
      if (!(b > a))
         return;
      double w = 0.0;
      for (const ImagerSource& L : imagers)
         w += L.rate * (DepthMean(a, b, L.shares) * (b - a));
      out.push_back({ 0.5 * (a + b), w });
   };
   if (tirf_)
   {
      double dMax = 0.0;
      for (const ImagerSource& L : imagers)
         for (const ExcitationLine& l : L.shares)
            dMax = std::max(dMax, DecayUm(l.nm));
      const double top = std::min(Hc, 30.0 * dMax);
      for (int i = 0; i < nTirf; i++)
         add(top * i / nTirf, top * (i + 1) / nTirf);
      return out;
   }
   const double f = std::min(Hc, std::max(0.0, zp_)), reach = std::max(f, Hc - f);
   double e0 = 0.0, e1 = kCollectDz0;
   while (e0 < reach)
   {
      add(std::max(0.0, f - e1), f - e0);
      add(f + e0, std::min(Hc, f + e1));
      e0 = e1;
      e1 *= ratio;
   }
   return out;
}

double Excitation::CollectedAt(double x, double y, const std::vector<Interval>& ivs, const CollectionRings& rings,
                               const std::vector<double>& ds, double small) const
{
   double s = 0.0;
   const size_t nr = rings.tans.size(), nd = ds.size() / 2;
   for (const Interval& iv : ivs)
   {
      if (iv.w == 0.0)
         continue;
      const double dz = std::fabs(iv.zm - zp_);
      double b = 0.0;
      for (size_t j = 0; j < nr; j++)
      {
         const double rho = dz * rings.tans[j];
         if (rho < small)
         {
            b += rings.weights[j] * Lateral(x, y, iv.zm);
            continue;
         }
         double r = 0.0;
         for (size_t k = 0; k < nd; k++)
            r += Lateral(x + rho * ds[2 * k], y + rho * ds[2 * k + 1], iv.zm);
         b += rings.weights[j] * (r / static_cast<double>(nd));
      }
      s += iv.w * b;
   }
   return F_ * s;
}

double Excitation::ImagerColumnAt(double x, double y, double Hc, const std::vector<ImagerSource>& imagers, int rings,
                                  int dirs, double ratio, int tirfSteps, bool fresnel) const
{
   if (clipped_ || !(Hc > 0.0))
      return 0.0;
   return CollectedAt(x, y, Intervals(Hc, imagers, ratio, tirfSteps), CollectionRingsFor(nS_, nI_, na_, rings, fresnel),
                      RingDirs(dirs), 0.0);
}

std::vector<double> Excitation::ImagerMap(unsigned W, unsigned H, double um, double axisX, double axisY, double Hc,
                                          const std::vector<ImagerSource>& imagers) const
{
   std::vector<double> out(static_cast<size_t>(W) * H, 0.0);
   if (clipped_ || !(Hc > 0.0) || imagers.empty())
      return out;
   if (profile_ == ILLUM_FLAT)
   {
      for (unsigned j = 0; j < H; j++)
         for (unsigned i = 0; i < W; i++)
         {
            double v = 0.0;
            for (const ImagerSource& L : imagers)
               v += L.rate * Column(i * um - axisX, j * um - axisY, Hc, L.shares);
            out[static_cast<size_t>(j) * W + i] = v;
         }
      return out;
   }
   const double scaleUm = profile_ == ILLUM_GAUSSIAN ? sig_ : edge_;
   const double g = std::max(um, scaleUm / 3.0);
   auto axis = [&](unsigned n) -> unsigned {
      if (n <= 1)
         return 1;
      return static_cast<unsigned>(
         std::min(static_cast<double>(kCollectNodes), std::max(2.0, std::ceil((n - 1) * um / g) + 1)));
   };
   const unsigned nx = axis(W), ny = axis(H);
   auto nodeAt = [&](unsigned k, unsigned nn, unsigned n) { return nn <= 1 ? 0.0 : (n - 1) * um * k / (nn - 1); };
   const std::vector<Interval> ivs = Intervals(Hc, imagers, kCollectRatio, kCollectTirfSteps);
   const double small = 0.125 * g;
   const CollectionRings rings = CollectionRingsFor(nS_, nI_, na_, 0, true);
   const std::vector<double> ds = RingDirs(kCollectDirs);
   std::vector<double> node(static_cast<size_t>(nx) * ny);
   ParallelFor(ny, [&](unsigned b) {
      for (unsigned a = 0; a < nx; a++)
         node[static_cast<size_t>(b) * nx + a] =
            CollectedAt(nodeAt(a, nx, W) - axisX, nodeAt(b, ny, H) - axisY, ivs, rings, ds, small);
   });
   // Pixel p of n on nn nodes: the node below it and the fraction towards the next.
   auto cellOf = [](unsigned p, unsigned n, unsigned nn, unsigned& k, double& fr) {
      if (nn <= 1)
      {
         k = 0;
         fr = 0.0;
         return;
      }
      const double f = static_cast<double>(p) * (nn - 1) / (n - 1);
      k = std::min(nn - 2, static_cast<unsigned>(std::floor(f)));
      fr = f - k;
   };
   for (unsigned j = 0; j < H; j++)
   {
      unsigned b = 0;
      double fy = 0.0;
      cellOf(j, H, ny, b, fy);
      const unsigned b1 = ny > 1 ? b + 1 : b;
      for (unsigned i = 0; i < W; i++)
      {
         unsigned a = 0;
         double fx = 0.0;
         cellOf(i, W, nx, a, fx);
         const unsigned a1 = nx > 1 ? a + 1 : a;
         const double v00 = node[static_cast<size_t>(b) * nx + a], v10 = node[static_cast<size_t>(b) * nx + a1];
         const double v01 = node[static_cast<size_t>(b1) * nx + a], v11 = node[static_cast<size_t>(b1) * nx + a1];
         out[static_cast<size_t>(j) * W + i] = (1.0 - fy) * ((1.0 - fx) * v00 + fx * v10) + fy * ((1.0 - fx) * v01 + fx * v11);
      }
   }
   return out;
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
