///////////////////////////////////////////////////////////////////////////////
// FILE:          Excitation.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Excitation.h; the same arithmetic as excitation.js, in its
//                operand order, for the legacy single beam (component 0: every
//                method loops over the components and a one-component loop is
//                that arithmetic bit for bit). Several beams: SimPattern.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Excitation.h"
#include "Parallel.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace sim {

namespace {
constexpr double kUGap = 1e-3;
constexpr double kFlatTopEdge = 0.15;   // the logistic edge width, x the disc radius (SMLMBackground.cpp)
constexpr double kPi = 3.14159265358979323846;
constexpr double kFwhmOverSigma = 2.3548;
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
   spec_ = s;
   comps_.clear();
   envs_.clear();
   patterns_.clear();
   state_ = PatternState();
   patterned_ = !s.beams.beams.empty();
   if (patterned_)
      MakePatterned(s);
   else
      MakeLegacy(s);
}

void Excitation::MakeLegacy(const ExcitationSpec& s)
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
   tilted_ = !tirf_ && tanS_ != 0.0;
   Component c;
   c.u = u_;
   c.cosI = cosI_;
   c.tanS = tanS_;
   c.F = F_;
   c.sx = tirf_ ? 0.0 : tanS_;
   c.sy = 0.0;
   c.reachUm = reachUm_;
   c.tirf = tirf_;
   c.clipped = clipped_;
   c.flat = profile_ == ILLUM_FLAT;
   c.curvature = profile_ == ILLUM_FLAT ? 0.0
                 : profile_ == ILLUM_GAUSSIAN ? 1.0 / (sig_ * sig_)
                                              : 1.0 / (6.0 * std::sqrt(3.0)) / (edge_ * edge_) / flatTop0_;
   comps_.push_back(c);
   Envelope e;
   e.fwhmUm = fwhmUm_;
   e.sig = sig_;
   e.r0 = r0_;
   e.edge = edge_;
   e.flatTop0 = flatTop0_;
   envs_.push_back(e);
}

double Excitation::SpotRadiusUm(double fwhmUm) const
{
   // A Gaussian envelope of FWHM f has 1/e^2 radius w_s = f / sqrt(2 ln 2); its spot in the pupil w_b = lambda f_obj /
   // (pi w_s).
   const double ws = std::max(1e-9, fwhmUm) / std::sqrt(2.0 * std::log(2.0));
   return spec_.lambdaRefNm * 1e-3 * spec_.fObjUm / (kPi * ws);
}

double Excitation::EnvelopeFwhmUm(double spotUm, double lambdaNm) const
{
   const double ws = lambdaNm * 1e-3 * spec_.fObjUm / (kPi * std::max(1e-9, spotUm));
   return ws * std::sqrt(2.0 * std::log(2.0));
}

void Excitation::MakePatterned(const ExcitationSpec& s)
{
   profile_ = s.profile;
   fwhmUm_ = std::max(1e-6, (s.fwhmPct / 100.0) * s.fovWidthUm);
   nS_ = s.nMed;
   nI_ = s.nImm;
   na_ = s.na;
   zp_ = s.pivotUm;
   uMax_ = s.na / nS_;
   sig_ = fwhmUm_ / 2.3548;
   r0_ = fwhmUm_ / 2.0;
   edge_ = std::max(1e-6, kFlatTopEdge * r0_);
   flatTop0_ = 1.0 / (1.0 + std::exp(-r0_ / edge_));
   uniform_ = false;
   // The cycle's components: every angle's beams, each at 1 / angles.
   const BeamSet& set = s.beams;
   const int A = std::max(1, set.angles);
   auto comp = [&](const Beam& b, int beam, double weight, Component& c, Envelope& e) {
      const BeamField f = BeamFieldOf(b, s.lambdaRefNm, nS_, nI_, na_, set.polarization);
      c.u = f.u;
      c.cosI = f.cosI;
      c.tanS = f.tanS;
      c.F = weight * f.Intensity();
      c.azimuth = b.AzimuthRad();
      c.sx = f.evanescent ? 0.0 : f.tanS * std::cos(c.azimuth);
      c.sy = f.evanescent ? 0.0 : f.tanS * std::sin(c.azimuth);
      c.ox = s.fObjUm * std::tan(b.tiltXDeg * kPi / 180.0);
      c.oy = s.fObjUm * std::tan(b.tiltYDeg * kPi / 180.0);
      c.tirf = f.evanescent;
      c.clipped = f.clipped;
      c.flat = profile_ == ILLUM_FLAT;
      c.beam = beam;
      const double fw = b.spotUm > 0 ? std::max(1e-6, EnvelopeFwhmUm(b.spotUm, s.lambdaRefNm)) : fwhmUm_;
      e.fwhmUm = fw;
      e.sig = fw / kFwhmOverSigma;
      e.r0 = fw / 2.0;
      e.edge = std::max(1e-6, kFlatTopEdge * e.r0);
      e.flatTop0 = 1.0 / (1.0 + std::exp(-e.r0 / e.edge));
      c.reachUm = c.flat ? std::numeric_limits<double>::infinity()
                         : (profile_ == ILLUM_GAUSSIAN ? e.sig * 7.5 : e.r0 + 28.0 * e.edge);
      c.curvature = c.flat ? 0.0
                    : profile_ == ILLUM_GAUSSIAN ? 1.0 / (e.sig * e.sig)
                                                 : 1.0 / (6.0 * std::sqrt(3.0)) / (e.edge * e.edge) / e.flatTop0;
   };
   for (int a = 0; a < A; ++a)
   {
      BeamSet one = set;
      one.angles = A;
      one.phases = 1;
      one.driftTurnsPerSqrtSec = 0;
      one.driftTurnsPerSec = 0;
      // The angle's rotation alone (frame a of a one-phase program of A angles).
      const PatternState st = PatternStateAt(one, a, 0, 0.0);
      for (size_t j = 0; j < st.beams.size(); ++j)
      {
         Component c;
         Envelope e;
         comp(st.beams[j], static_cast<int>(j), 1.0 / A, c, e);
         comps_.push_back(c);
         envs_.push_back(e);
      }
   }
   // The outermost beam's regime and the readouts.
   int outer = -1;
   double uo = -1.0;
   bool allClipped = true;
   F_ = 0.0;
   for (size_t j = 0; j < comps_.size(); ++j)
   {
      allClipped = allClipped && comps_[j].clipped;
      if (!comps_[j].clipped)
         F_ += comps_[j].F;
      if (comps_[j].u > uo)
      {
         uo = comps_[j].u;
         outer = static_cast<int>(j);
      }
   }
   clipped_ = allClipped;
   const Component& o = comps_[static_cast<size_t>(std::max(0, outer))];
   u_ = o.u;
   cosI_ = o.cosI;
   tanS_ = o.tanS;
   tirf_ = o.tirf && !o.clipped;
   regime_ = o.clipped ? REGIME_CLIPPED : (tirf_ ? REGIME_TIRF : (u_ > 0.0 ? REGIME_HILO : REGIME_EPI));
   reachUm_ = o.reachUm;
   tilted_ = false;
   for (const Component& c : comps_)
      tilted_ = tilted_ || (!c.tirf && !c.clipped && c.tanS != 0.0);
   SetFrame(s.frame);
}

void Excitation::SetFrame(long long frame)
{
   if (!patterned_)
      return;
   spec_.frame = frame;
   state_ = PatternStateAt(spec_.beams, frame, spec_.seed, spec_.expSec);
   Compile();
}

void Excitation::Compile()
{
   patterns_.clear();
   std::vector<double> lines = spec_.lineNm;
   if (lines.empty())
      lines.push_back(spec_.lambdaRefNm);
   for (double nm : lines)
      patterns_.push_back({ nm, CompilePattern(state_, nm, nS_, nI_, na_, spec_.beams.polarization) });
}

const Pattern* Excitation::PatternOf(double nm) const
{
   for (const auto& p : patterns_)
      if (p.first == nm)
         return &p.second;
   // The nearest compiled line (a custom line the spec did not list).
   const Pattern* best = nullptr;
   double dBest = std::numeric_limits<double>::infinity();
   for (const auto& p : patterns_)
      if (std::fabs(p.first - nm) < dBest)
      {
         dBest = std::fabs(p.first - nm);
         best = &p.second;
      }
   return best;
}

std::vector<double> Excitation::PeriodsUm(double nm) const
{
   const Pattern* p = PatternOf(nm);
   return p ? p->PeriodsUm() : std::vector<double>();
}

double Excitation::EnvelopeAt(const Envelope& e, double xp, double y) const
{
   if (profile_ == ILLUM_FLAT)
      return 1.0;
   const double r2 = xp * xp + y * y;
   if (profile_ == ILLUM_GAUSSIAN)
      return std::exp(-r2 / (2.0 * e.sig * e.sig));
   return 1.0 / (1.0 + std::exp((std::sqrt(r2) - e.r0) / e.edge)) / e.flatTop0;
}

double Excitation::ProfileAt(double xp, double y) const
{
   return EnvelopeAt(envs_[0], xp, y);
}

double Excitation::ComponentDecayUm(int j, double nm) const
{
   const Component& c = comps_[static_cast<size_t>(j)];
   return 1e-3 * nm / (4.0 * kPi * nS_ * std::sqrt(c.u * c.u - 1.0));
}

double Excitation::DecayUm(double nm) const
{
   return 1e-3 * nm / (4.0 * kPi * nS_ * std::sqrt(u_ * u_ - 1.0));
}

double Excitation::ReachUm(double frac) const
{
   double r = 0.0;
   for (size_t j = 0; j < comps_.size(); ++j)
   {
      const Component& c = comps_[j];
      const Envelope& e = envs_[j];
      double rj;
      if (c.flat)
         rj = std::numeric_limits<double>::infinity();
      else if (profile_ == ILLUM_GAUSSIAN)
         rj = e.sig * std::sqrt(-2.0 * std::log(frac));
      else
         rj = e.r0 + e.edge * std::log(1.0 / (frac * e.flatTop0) - 1.0);
      r = std::max(r, rj + std::hypot(c.ox, c.oy));
   }
   return r;
}

double Excitation::MaxCurvature() const
{
   double k = 0.0;
   for (const Component& c : comps_)
      k = std::max(k, c.curvature);
   return k;
}

double Excitation::ComponentLateral(int j, double x, double y, double z) const
{
   const Component& c = comps_[static_cast<size_t>(j)];
   const Envelope& e = envs_[static_cast<size_t>(j)];
   if (c.azimuth == 0.0 && c.ox == 0.0 && c.oy == 0.0)
      return EnvelopeAt(e, (x - (c.tirf ? 0.0 : z - zp_) * c.tanS) * c.cosI, y);
   const double dx = x - c.ox, dy = y - c.oy;
   const double ca = std::cos(c.azimuth), sa = std::sin(c.azimuth);
   const double xr = dx * ca + dy * sa, yr = -dx * sa + dy * ca;
   return EnvelopeAt(e, (xr - (c.tirf ? 0.0 : z - zp_) * c.tanS) * c.cosI, yr);
}

double Excitation::Lateral(double x, double y, double z) const
{
   return ComponentLateral(0, x, y, z);
}

double Excitation::ComponentDepth(int j, double z, const ExcitationShares& shares) const
{
   const Component& c = comps_[static_cast<size_t>(j)];
   if (!c.tirf)
      return 1.0;
   double s = 0.0;
   for (const ExcitationLine& l : shares)
      s += l.share * std::exp(-std::max(0.0, z) / ComponentDecayUm(j, l.nm));
   return s;
}

double Excitation::Depth(double z, const ExcitationShares& shares) const
{
   return ComponentDepth(0, z, shares);
}

double Excitation::ComponentDepthMean(int j, double z0, double z1, const ExcitationShares& shares) const
{
   const Component& c = comps_[static_cast<size_t>(j)];
   if (!c.tirf)
      return 1.0;
   const double a = std::max(0.0, z0), b = std::max(a, z1);
   if (!(b > a))
      return ComponentDepth(j, a, shares);
   double s = 0.0;
   for (const ExcitationLine& l : shares)
   {
      const double d = ComponentDecayUm(j, l.nm);
      s += l.share * d * (std::exp(-a / d) - std::exp(-b / d));
   }
   return s / (b - a);
}

double Excitation::DepthMean(double z0, double z1, const ExcitationShares& shares) const
{
   return ComponentDepthMean(0, z0, z1, shares);
}

double Excitation::MeanAt(double x, double y, double z, const ExcitationShares& shares) const
{
   if (clipped_)
      return 0.0;
   double s = 0.0;
   for (size_t j = 0; j < comps_.size(); ++j)
   {
      const Component& c = comps_[j];
      if (c.clipped)
         continue;
      s += c.F * ComponentLateral(static_cast<int>(j), x, y, z) * ComponentDepth(static_cast<int>(j), z, shares);
   }
   return s;
}

double Excitation::MeanPlaneMean(double x, double y, double z0, double z1, const ExcitationShares& shares) const
{
   if (clipped_)
      return 0.0;
   const double zc = std::max(0.0, (z0 + z1) / 2);
   double s = 0.0;
   for (size_t j = 0; j < comps_.size(); ++j)
   {
      const Component& c = comps_[j];
      if (c.clipped)
         continue;
      s += c.F * ComponentLateral(static_cast<int>(j), x, y, zc) * ComponentDepthMean(static_cast<int>(j), z0, z1, shares);
   }
   return s;
}

double Excitation::At(double x, double y, double z, const ExcitationShares& shares) const
{
   if (!patterned_)
      return clipped_ ? 0.0 : F_ * Lateral(x, y, z) * Depth(z, shares);
   if (clipped_)
      return 0.0;
   // The frame's envelopes: the state's beam j is the cycle component (angle index x N + j) rotated to the frame's
   // angle; its envelope is that component's with the frame's azimuth. The components of angle `light.angle` are
   // exactly the frame's beams (the same rotation), so use them.
   const size_t N = state_.beams.size();
   const size_t base = static_cast<size_t>(state_.light.angle) * N;
   std::vector<double> env(N);
   for (size_t j = 0; j < N; ++j)
   {
      const size_t cj = base + j < comps_.size() ? base + j : j;
      env[j] = comps_[cj].clipped ? 0.0 : ComponentLateral(static_cast<int>(cj), x, y, z);
   }
   double s = 0.0;
   for (const ExcitationLine& l : shares)
   {
      const Pattern* p = PatternOf(l.nm);
      if (!p)
         continue;
      s += l.share * p->At(x, y, z, env.data());
   }
   return s;
}

namespace {
// The mean of exp(-z / d) cos(theta + kz z) over [a, b] (d 0: no decay); b <= a: the value at a.
double DampedCosMean(double d, double kz, double theta, double a, double b)
{
   const double sr = d > 0 ? -1.0 / d : 0.0;
   if (!(b > a))
      return std::exp(sr * a) * std::cos(theta + kz * a);
   const std::complex<double> s(sr, kz), I(0.0, 1.0);
   std::complex<double> integral;
   if (std::abs(s) < 1e-12)
      integral = b - a;
   else
      integral = (std::exp(s * b) - std::exp(s * a)) / s;
   return std::real(std::exp(I * theta) * integral) / (b - a);
}
double Sinc(double x)
{
   return std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
}
} // namespace

double Excitation::PlaneMean(double x, double y, double z0, double z1, double cellUm,
                             const ExcitationShares& shares) const
{
   if (!patterned_)
   {
      if (clipped_)
         return 0.0;
      return F_ * Lateral(x, y, std::max(0.0, (z0 + z1) / 2)) * DepthMean(z0, z1, shares);
   }
   if (clipped_)
      return 0.0;
   const double a = std::max(0.0, z0), b = std::max(a, z1), zc = std::max(0.0, (z0 + z1) / 2);
   const size_t N = state_.beams.size();
   const size_t base = static_cast<size_t>(state_.light.angle) * N;
   std::vector<double> env(N);
   for (size_t j = 0; j < N; ++j)
   {
      const size_t cj = base + j < comps_.size() ? base + j : j;
      env[j] = comps_[cj].clipped ? 0.0 : ComponentLateral(static_cast<int>(cj), x, y, zc);
   }
   double s = 0.0;
   for (const ExcitationLine& l : shares)
   {
      const Pattern* p = PatternOf(l.nm);
      if (!p)
         continue;
      double v = 0.0;
      for (const PatternTerm& t : p->terms)
      {
         double e = t.j == t.l ? env[static_cast<size_t>(t.j)]
                               : std::sqrt(env[static_cast<size_t>(t.j)] * env[static_cast<size_t>(t.l)]);
         if (!(e > 0))
            continue;
         if (t.j == t.l)
            e *= t.decayUm > 0 ? DampedCosMean(t.decayUm, 0.0, 0.0, a, b) : 1.0;
         else
         {
            e *= DampedCosMean(t.decayUm, t.kz, t.qx * x + t.qy * y + t.phase, a, b);
            if (cellUm > 0)
               e *= Sinc(t.qx * cellUm / 2) * Sinc(t.qy * cellUm / 2);
         }
         v += t.amp * e;
      }
      s += l.share * v;
   }
   return s;
}

double Excitation::Column(double x, double y, double H, const ExcitationShares& shares) const
{
   if (clipped_ || !(H > 0.0))
      return 0.0;
   double total = 0.0;
   for (size_t j = 0; j < comps_.size(); ++j)
   {
      const Component& c = comps_[j];
      if (c.clipped)
         continue;
      const int ji = static_cast<int>(j);
      if (c.tirf)
      {
         double s = 0.0;
         for (const ExcitationLine& l : shares)
         {
            const double d = ComponentDecayUm(ji, l.nm);
            s += l.share * d * (1.0 - std::exp(-H / d));
         }
         total += c.F * ComponentLateral(ji, x, y, 0.0) * s;
         continue;
      }
      if (c.tanS == 0.0 || c.flat)
      {
         total += c.F * ComponentLateral(ji, x, y, 0.0) * H;
         continue;
      }
      // The sheet crosses the column where its footprint reaches: along the beam's azimuth.
      const double r = c.reachUm / c.cosI;
      const double ca = std::cos(c.azimuth), sa = std::sin(c.azimuth);
      const double xr = (x - c.ox) * ca + (y - c.oy) * sa;
      const double za = std::max(0.0, zp_ + (xr - r) / c.tanS), zb = std::min(H, zp_ + (xr + r) / c.tanS);
      if (!(zb > za))
         continue;
      const int n = 256;
      const double dz = (zb - za) / n;
      double s = 0.0;
      for (int i = 0; i < n; i++)
         s += ComponentLateral(ji, x, y, za + (i + 0.5) * dz);
      total += c.F * s * dz;
   }
   return total;
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
   // The intervals of component 0 (the legacy beam; a beam set: every component is at the same focus, and the TIRF
   // decays are those of the components themselves: CollectedAt weighs each with its own depth mean).
   std::vector<Interval> out;
   const Component& c = comps_[0];
   auto add = [&](double a, double b) {
      if (!(b > a))
         return;
      double w = 0.0;
      for (const ImagerSource& L : imagers)
         w += L.rate * (ComponentDepthMean(0, a, b, L.shares) * (b - a));
      out.push_back({ 0.5 * (a + b), w });
   };
   if (c.tirf)
   {
      double dMax = 0.0;
      for (const ImagerSource& L : imagers)
         for (const ExcitationLine& l : L.shares)
            dMax = std::max(dMax, ComponentDecayUm(0, l.nm));
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
   // Component 0's collected light with component 0's intervals (the legacy path, bit for bit); a beam set adds the
   // other components over the same intervals (their weights component 0's depth means: components at other TIRF
   // depths are approximate here -- the imager background is on the cycle mean anyway, spec/ALGORITHM.md).
   const size_t nr = rings.tans.size(), nd = ds.size() / 2;
   double total = 0.0;
   for (size_t j = 0; j < comps_.size(); ++j)
   {
      const Component& c = comps_[j];
      if (c.clipped)
         continue;
      const int ji = static_cast<int>(j);
      double s = 0.0;
      for (const Interval& iv : ivs)
      {
         if (iv.w == 0.0)
            continue;
         const double dz = std::fabs(iv.zm - zp_);
         double b = 0.0;
         for (size_t r = 0; r < nr; ++r)
         {
            const double rho = dz * rings.tans[r];
            if (rho < small)
            {
               b += rings.weights[r] * ComponentLateral(ji, x, y, iv.zm);
               continue;
            }
            double rr = 0.0;
            for (size_t k = 0; k < nd; k++)
               rr += ComponentLateral(ji, x + rho * ds[2 * k], y + rho * ds[2 * k + 1], iv.zm);
            b += rings.weights[r] * (rr / static_cast<double>(nd));
         }
         s += iv.w * b;
      }
      total += c.F * s;
   }
   return total;
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
   double scaleUm = profile_ == ILLUM_GAUSSIAN ? envs_[0].sig : envs_[0].edge;
   for (const Envelope& e : envs_)
      scaleUm = std::min(scaleUm, profile_ == ILLUM_GAUSSIAN ? e.sig : e.edge);
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

} // namespace sim
