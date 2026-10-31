///////////////////////////////////////////////////////////////////////////////
// FILE:          SimPattern.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See SimPattern.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "SimPattern.h"
#include "SMLMCounterRng.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kUGap = 1e-3;   // u keeps this away from 1 (Excitation.cpp: the plane-wave limits diverge there)
constexpr uint32_t kPhaseSeedSalt = 0x53494D50u;   // "SIMP"
} // namespace

const std::vector<std::string>& SimTypeNames()
{
   static const std::vector<std::string> v = { "Off", "SIM2D", "SIM3D", "LatticeSIM2D", "LatticeSIM3D", "Custom" };
   return v;
}

const std::vector<std::string>& SimPolarizationNames()
{
   static const std::vector<std::string> v = { "S", "P", "Circular", "LinearX", "Unpolarized" };
   return v;
}

const std::vector<std::string>& SimOrderNames()
{
   static const std::vector<std::string> v = { "AngleMajor", "PhaseMajor" };
   return v;
}

const std::vector<std::string>& SimAnchorNames()
{
   static const std::vector<std::string> v = { "Focus", "Coverslip" };
   return v;
}

double SimTypeDefaultRadius(int type)
{
   switch (type)
   {
   case SIM_2D: return 0.95;
   case SIM_3D:
   case SIM_LATTICE_2D:
   case SIM_LATTICE_3D: return 0.9;
   default: return 0.0;
   }
}

double Beam::Radius() const { return std::sqrt(ux * ux + uy * uy); }
double Beam::AzimuthRad() const { return (ux == 0.0 && uy == 0.0) ? 0.0 : std::atan2(uy, ux); }

const std::vector<std::string>& BeamColumnNames()
{
   static const std::vector<std::string> v = { "ux", "uy", "spot", "tiltX", "tiltY", "amp", "phase", "pol", "ellip",
                                               "phaseStep", "nm" };
   return v;
}

double BeamColumn(const Beam& b, int c)
{
   switch (c)
   {
   case 0: return b.ux;
   case 1: return b.uy;
   case 2: return b.spotUm;
   case 3: return b.tiltXDeg;
   case 4: return b.tiltYDeg;
   case 5: return b.amp;
   case 6: return b.phaseTurns;
   case 7: return b.polDeg;
   case 8: return b.ellipTurns;
   case 9: return b.phaseStep;
   case 10: return b.nm;
   default: return 0.0;
   }
}

void SetBeamColumn(Beam& b, int c, double v)
{
   switch (c)
   {
   case 0: b.ux = v; break;
   case 1: b.uy = v; break;
   case 2: b.spotUm = v; break;
   case 3: b.tiltXDeg = v; break;
   case 4: b.tiltYDeg = v; break;
   case 5: b.amp = v; break;
   case 6: b.phaseTurns = v; break;
   case 7: b.polDeg = v; break;
   case 8: b.ellipTurns = v; break;
   case 9: b.phaseStep = v; break;
   case 10: b.nm = v; break;
   default: break;
   }
}

namespace {
void Polarize(Beam& b, int polarization)
{
   b.ellipTurns = 0.0;
   switch (polarization)
   {
   case SIM_POL_P: b.polDeg = 90.0; break;
   case SIM_POL_CIRCULAR:
      b.polDeg = 45.0;
      b.ellipTurns = 0.25;
      break;
   case SIM_POL_LINEAR_X:
   {
      // The pupil field along the lab x axis: its s part -sin phi, its p part cos phi.
      const double phi = b.AzimuthRad();
      b.polDeg = std::atan2(std::cos(phi), -std::sin(phi)) * 180.0 / kPi;
      break;
   }
   case SIM_POL_UNPOLARIZED: b.polDeg = kUnpolarizedDeg; break;
   default: b.polDeg = 0.0; break;
   }
}
} // namespace

BeamSet MakeBeams(int type, double radiusU, int polarization, double centreAmp)
{
   BeamSet s;
   s.type = type;
   s.polarization = polarization;
   const double r = std::max(0.0, radiusU);
   auto beam = [&](double ux, double uy, double amp, double step) {
      Beam b;
      b.ux = ux;
      b.uy = uy;
      b.amp = amp;
      b.phaseStep = step;
      Polarize(b, polarization);
      s.beams.push_back(b);
   };
   switch (type)
   {
   case SIM_2D:
      beam(r, 0, 1, 1);
      beam(-r, 0, 1, -1);
      s.phases = 3;
      s.angles = 3;
      break;
   case SIM_3D:
      beam(0, 0, centreAmp, 0);
      beam(r, 0, 1, 1);
      beam(-r, 0, 1, -1);
      s.phases = 5;
      s.angles = 3;
      break;
   case SIM_LATTICE_2D:
      beam(r, 0, 1, 1);
      beam(-r, 0, 1, -1);
      beam(0, r, 1, 2);
      beam(0, -r, 1, -2);
      s.phases = 9;
      s.angles = 1;
      break;
   case SIM_LATTICE_3D:
      beam(0, 0, centreAmp, 0);
      // The centre beam circular under S and P (the user's choice, 2026-10-30): the x and y pairs do not interfere
      // with each other, and the centre beam interferes with both alike.
      if (polarization == SIM_POL_S || polarization == SIM_POL_P)
      {
         s.beams.back().polDeg = 45.0;
         s.beams.back().ellipTurns = 0.25;
      }
      beam(r, 0, 1, 2);
      beam(-r, 0, 1, -2);
      beam(0, r, 1, 3);
      beam(0, -r, 1, -3);
      s.phases = 13;
      s.angles = 1;
      break;
   case SIM_CUSTOM: break;
   default:
      beam(r, 0, 1, 0);
      break;
   }
   return s;
}

void ApplyBeamRow(BeamSet& set, int j, const std::vector<std::pair<int, double>>& columns)
{
   if (j < 0)
      return;
   while (static_cast<int>(set.beams.size()) <= j)
   {
      Beam b;
      Polarize(b, set.polarization);
      set.beams.push_back(b);
   }
   for (const auto& cv : columns)
      if (cv.first >= 0 && cv.first < kBeamColumns)
         SetBeamColumn(set.beams[static_cast<size_t>(j)], cv.first, cv.second);
}

bool ParseBeamMatrix(const std::string& text, std::vector<std::vector<double>>& rows, std::string& err)
{
   rows.clear();
   size_t i = 0;
   while (i < text.size())
   {
      size_t j = i;
      while (j < text.size() && text[j] != ';' && text[j] != '\n')
         j++;
      const std::string row = text.substr(i, j - i);
      std::vector<double> vals;
      size_t a = 0;
      while (a < row.size())
      {
         while (a < row.size() && (row[a] == ' ' || row[a] == ',' || row[a] == '\t' || row[a] == '\r'))
            a++;
         size_t b = a;
         while (b < row.size() && row[b] != ' ' && row[b] != ',' && row[b] != '\t' && row[b] != '\r')
            b++;
         if (b == a)
            break;
         const std::string tok = row.substr(a, b - a);
         char* end = nullptr;
         const double v = std::strtod(tok.c_str(), &end);
         if (end == tok.c_str() || *end != 0 || !std::isfinite(v))
         {
            err = "beam matrix: bad value '" + tok + "'";
            return false;
         }
         vals.push_back(v);
         a = b;
      }
      if (vals.size() > static_cast<size_t>(kBeamColumns))
      {
         err = "beam matrix: a row has more than " + std::to_string(kBeamColumns) + " values";
         return false;
      }
      if (!vals.empty())
         rows.push_back(vals);
      i = j + 1;
   }
   return true;
}

std::string FormatBeamMatrix(const std::vector<Beam>& beams)
{
   std::string s;
   for (size_t j = 0; j < beams.size(); ++j)
   {
      if (j)
         s += "; ";
      for (int c = 0; c < kBeamColumns; ++c)
      {
         char b[32];
         std::snprintf(b, sizeof b, "%.6g", BeamColumn(beams[j], c));
         if (c)
            s += ' ';
         s += b;
      }
   }
   return s;
}

FrameLight FrameLightAt(const BeamSet& set, long long frame)
{
   FrameLight L;
   const long long P = std::max(1, set.phases), A = std::max(1, set.angles);
   const long long f = ((frame % (P * A)) + P * A) % (P * A);
   if (set.order == SIM_ORDER_PHASE_MAJOR)
   {
      L.angle = static_cast<int>(f % A);
      L.phase = static_cast<int>((f / A) % P);
   }
   else
   {
      L.phase = static_cast<int>(f % P);
      L.angle = static_cast<int>((f / P) % A);
   }
   return L;
}

double PhaseDriftTurns(const BeamSet& set, uint32_t seed, int beam, long long frame, double expSec)
{
   if (beam < 0 || beam >= static_cast<int>(set.beams.size()))
      return 0.0;
   double d = 0.0;
   if (set.driftTurnsPerSqrtSec > 0 && expSec > 0)
   {
      const double step = set.driftTurnsPerSqrtSec * std::sqrt(expSec);
      const uint32_t s = seed ^ kPhaseSeedSalt;
      for (long long i = 0; i < frame; ++i)
      {
         CounterRng u(s, static_cast<uint32_t>(i));
         u.Pixel(static_cast<uint32_t>(beam));
         d += step * CounterGauss(u);
      }
   }
   if (set.driftTurnsPerSec != 0)
      d += set.beams[static_cast<size_t>(beam)].phaseStep * set.driftTurnsPerSec * frame * expSec;
   return d;
}

PatternState PatternStateAt(const BeamSet& set, long long frame, uint32_t seed, double expSec)
{
   PatternState st;
   st.frame = frame;
   st.light = FrameLightAt(set, frame);
   st.rotDeg = set.angle0Deg + st.light.angle * 180.0 / std::max(1, set.angles);
   const double c = std::cos(st.rotDeg * kPi / 180.0), s = std::sin(st.rotDeg * kPi / 180.0);
   for (size_t j = 0; j < set.beams.size(); ++j)
   {
      Beam b = set.beams[j];
      const double ux = b.ux, uy = b.uy, tx = b.tiltXDeg, ty = b.tiltYDeg;
      b.ux = c * ux - s * uy;
      b.uy = s * ux + c * uy;
      b.tiltXDeg = c * tx - s * ty;
      b.tiltYDeg = s * tx + c * ty;
      double ph = b.phaseTurns + b.phaseStep * (st.light.phase + set.phase0) / std::max(1, set.phases);
      if (set.driftTurnsPerSqrtSec > 0 || set.driftTurnsPerSec != 0)
         ph += PhaseDriftTurns(set, seed, static_cast<int>(j), frame, expSec);
      b.phaseTurns = ph;
      st.phaseTurns.push_back(ph);
      st.beams.push_back(b);
   }
   return st;
}

BeamField BeamFieldOf(const Beam& b, double lambdaNm, double nS, double nI, double na, int polarization,
                      bool forceS, bool forceP)
{
   (void)polarization;
   BeamField f;
   double u = b.Radius();
   const double uMax = na / nS;
   f.clipped = u > uMax;
   if (std::fabs(u - 1.0) < kUGap)
      u = u < 1.0 ? 1.0 - kUGap : 1.0 + kUGap;
   f.u = u;
   f.evanescent = !f.clipped && u > 1.0;
   const double sinI = u * nS / nI;
   f.cosI = std::sqrt(std::max(0.0, 1.0 - sinI * sinI));
   const double n = nS / nI;
   const double t0 = 2.0 * nI / (nI + nS);
   std::complex<double> Es(0.0, 0.0), Epx(0.0, 0.0), Epz(0.0, 0.0);   // s along y; p: x (lateral), z
   if (f.clipped)
      return f;
   if (f.evanescent)
   {
      f.sinS = u;
      f.cosS = 0.0;
      f.tanS = 0.0;
      f.kzOverK = 0.0;
      f.decayUm = 1e-3 * lambdaNm / (4.0 * kPi * nS * std::sqrt(u * u - 1.0));
      // Axelrod 1984 (eqs. 3-8), the incidence angle theta in the immersion: amplitudes over the incident one,
      // phases delta_s, delta_p (sin^2 theta - n^2 > 0 beyond the critical angle).
      const double c = f.cosI, s2 = sinI * sinI, q = std::sqrt(std::max(0.0, s2 - n * n));
      const double ds = std::atan(q / c), dp = std::atan(q / (n * n * c));
      const std::complex<double> I(0.0, 1.0);
      Es = 2.0 * c / std::sqrt(1.0 - n * n) * std::exp(-I * ds);
      const double den = std::sqrt(n * n * n * n * c * c + s2 - n * n);
      Epx = 2.0 * c * q / den * std::exp(-I * (dp + kPi / 2.0));
      Epz = 2.0 * c * sinI / den * std::exp(-I * dp);
   }
   else
   {
      f.sinS = u;
      f.cosS = std::sqrt(1.0 - u * u);
      f.tanS = f.sinS / f.cosS;
      f.kzOverK = f.cosS;
      const double ts = 2.0 * nI * f.cosI / (nI * f.cosI + nS * f.cosS), tp = 2.0 * nI * f.cosI / (nS * f.cosI + nI * f.cosS);
      Es = ts;
      Epx = tp * f.cosS;
      Epz = -tp * f.sinS;
   }
   double a = b.polDeg;
   if (forceS)
      a = 0.0;
   else if (forceP || a < kUnpolarizedBelowDeg)
      a = forceP ? 90.0 : 0.0;
   const double ca = std::cos(a * kPi / 180.0), sa = std::sin(a * kPi / 180.0);
   const std::complex<double> ep = std::polar(1.0, 2.0 * kPi * b.ellipTurns);
   f.ey = b.amp * ca * Es / t0;
   f.ex = b.amp * sa * ep * Epx / t0;
   f.ez = b.amp * sa * ep * Epz / t0;
   return f;
}

namespace {
// The field of a beam in the lab frame (its own frame rotated by its azimuth).
void LabField(const BeamField& f, double phi, std::complex<double>& Ex, std::complex<double>& Ey,
              std::complex<double>& Ez)
{
   const double c = std::cos(phi), s = std::sin(phi);
   Ex = f.ex * c - f.ey * s;
   Ey = f.ex * s + f.ey * c;
   Ez = f.ez;
}

void CompileRun(const PatternState& st, double lambdaNm, double nS, double nI, double na, bool forceS, bool forceP,
                double weight, double zRefUm, Pattern& out, bool keepFields)
{
   const double k = 2.0 * kPi * nS / (lambdaNm * 1e-3);   // rad/um in the medium
   const size_t N = st.beams.size();
   std::vector<BeamField> F(N);
   std::vector<std::complex<double>> Ex(N), Ey(N), Ez(N);
   std::vector<bool> on(N, false);
   for (size_t j = 0; j < N; ++j)
   {
      const Beam& b = st.beams[j];
      F[j] = BeamFieldOf(b, lambdaNm, nS, nI, na, 0, forceS, forceP);
      on[j] = !F[j].clipped && (b.nm <= 0.0 || std::fabs(b.nm - lambdaNm) < 0.5) && b.amp != 0.0;
      LabField(F[j], b.AzimuthRad(), Ex[j], Ey[j], Ez[j]);
   }
   if (keepFields)
      out.fields = F;
   for (size_t j = 0; j < N; ++j)
   {
      if (!on[j])
         continue;
      PatternTerm t;
      t.j = t.l = static_cast<int>(j);
      t.decayUm = F[j].decayUm;
      t.amp = weight * (std::norm(Ex[j]) + std::norm(Ey[j]) + std::norm(Ez[j]));
      out.terms.push_back(t);
      for (size_t l = j + 1; l < N; ++l)
      {
         if (!on[l])
            continue;
         const std::complex<double> c = Ex[j] * std::conj(Ex[l]) + Ey[j] * std::conj(Ey[l]) + Ez[j] * std::conj(Ez[l]);
         if (std::abs(c) == 0.0)
            continue;
         PatternTerm p;
         p.j = static_cast<int>(j);
         p.l = static_cast<int>(l);
         p.qx = k * (st.beams[j].ux - st.beams[l].ux);
         p.qy = k * (st.beams[j].uy - st.beams[l].uy);
         p.kz = k * (F[j].kzOverK - F[l].kzOverK);
         double inv = 0.0;
         if (F[j].decayUm > 0)
            inv += 1.0 / (2.0 * F[j].decayUm);
         if (F[l].decayUm > 0)
            inv += 1.0 / (2.0 * F[l].decayUm);
         p.decayUm = inv > 0 ? 1.0 / inv : 0.0;
         p.amp = weight * 2.0 * std::abs(c);
         // The beams in phase at zRefUm (the focal plane or the coverslip): the fringe's axial phase referred there.
         p.phase = 2.0 * kPi * (st.beams[j].phaseTurns - st.beams[l].phaseTurns) + std::arg(c) - p.kz * zRefUm;
         out.terms.push_back(p);
      }
   }
}
} // namespace

Pattern CompilePattern(const PatternState& st, double lambdaNm, double nS, double nI, double na, int polarization,
                       double zRefUm)
{
   Pattern out;
   bool unpol = polarization == SIM_POL_UNPOLARIZED;
   for (const Beam& b : st.beams)
      unpol = unpol || b.polDeg < kUnpolarizedBelowDeg;
   if (unpol)
   {
      CompileRun(st, lambdaNm, nS, nI, na, true, false, 0.5, zRefUm, out, true);
      CompileRun(st, lambdaNm, nS, nI, na, false, true, 0.5, zRefUm, out, false);
   }
   else
      CompileRun(st, lambdaNm, nS, nI, na, false, false, 1.0, zRefUm, out, true);
   return out;
}

std::vector<double> Pattern::PeriodsUm() const
{
   std::vector<double> p;
   for (const PatternTerm& t : terms)
   {
      if (t.j == t.l)
         continue;
      const double q = std::sqrt(t.qx * t.qx + t.qy * t.qy);
      if (!(q > 0))
         continue;
      const double per = 2.0 * kPi / q;
      bool have = false;
      for (double v : p)
         have = have || std::fabs(v - per) < 1e-9 * per;
      if (!have)
         p.push_back(per);
   }
   std::sort(p.begin(), p.end(), std::greater<double>());
   return p;
}

double Pattern::At(double x, double y, double z, const double* env) const
{
   double s = 0.0;
   for (const PatternTerm& t : terms)
   {
      double e = t.j == t.l ? env[t.j] : std::sqrt(env[t.j] * env[t.l]);
      if (!(e > 0))
         continue;
      if (t.decayUm > 0)
         e *= std::exp(-std::max(0.0, z) / t.decayUm);
      if (t.j != t.l)
         e *= std::cos(t.qx * x + t.qy * y + t.kz * z + t.phase);
      s += t.amp * e;
   }
   return s;
}

namespace {
// The mean of exp(i k . r) over the unit ball, for |k| = k (complex k allowed: a decaying wave).
std::complex<double> BallFormFactor(std::complex<double> k)
{
   const std::complex<double> k2 = k * k;
   if (std::abs(k) < 1e-3)
      return 1.0 - k2 / 10.0 + k2 * k2 / 280.0;
   return 3.0 * (std::sin(k) - k * std::cos(k)) / (k2 * k);
}
double BallMeanTerms(const std::vector<PatternTerm>& terms, bool dcOnly, double x, double y, double z, double r,
                     const double* env)
{
   const std::complex<double> I(0.0, 1.0);
   double s = 0.0;
   for (const PatternTerm& t : terms)
   {
      const bool fringe = t.j != t.l;
      if (dcOnly && fringe)
         continue;
      double e = fringe ? std::sqrt(env[t.j] * env[t.l]) : env[t.j];
      if (!(e > 0))
         continue;
      const double sr = t.decayUm > 0 ? 1.0 / t.decayUm : 0.0;   // exp(-sr z) = Re exp(i (i sr) z)
      const std::complex<double> kzc(fringe ? t.kz : 0.0, sr);
      const std::complex<double> k = r * std::sqrt(std::complex<double>(fringe ? t.qx * t.qx + t.qy * t.qy : 0.0) + kzc * kzc);
      const double theta = fringe ? t.qx * x + t.qy * y + t.kz * z + t.phase : 0.0;
      const std::complex<double> v = std::exp(I * theta - sr * std::max(0.0, z)) * BallFormFactor(k);
      s += t.amp * e * v.real();
   }
   return s;
}
} // namespace

double Pattern::BallMean(double x, double y, double z, double r, const double* env) const
{
   return r > 0 ? BallMeanTerms(terms, false, x, y, z, r, env) : At(x, y, z, env);
}

double Pattern::DcBallMean(double z, double r, const double* env) const
{
   return BallMeanTerms(terms, true, 0.0, 0.0, z, std::max(0.0, r), env);
}

bool OrdersSeparate(const BeamSet& set, std::string& why)
{
   why.clear();
   const int P = std::max(1, set.phases);
   const size_t N = set.beams.size();
   // Orders: lateral q up to sign, keyed on (ux_j - ux_l, uy_j - uy_l) rounded; each must get one shift class.
   std::map<std::pair<long, long>, int> shift;
   auto key = [](double dx, double dy) {
      long a = std::lround(dx * 1e6), b = std::lround(dy * 1e6);
      if (a < 0 || (a == 0 && b < 0))
      {
         a = -a;
         b = -b;
      }
      return std::make_pair(a, b);
   };
   auto mod = [&](long v) { return static_cast<int>(((v % P) + P) % P); };
   std::map<int, std::pair<long, long>> seen;   // shift class -> the order that has it
   seen[0] = { 0, 0 };
   for (size_t j = 0; j < N; ++j)
      for (size_t l = j + 1; l < N; ++l)
      {
         const Beam &a = set.beams[j], &b = set.beams[l];
         if (a.amp == 0.0 || b.amp == 0.0)
            continue;
         double dx = a.ux - b.ux, dy = a.uy - b.uy;
         long ds = std::lround(a.phaseStep - b.phaseStep);
         if (dx < 0 || (dx == 0 && dy < 0))
         {
            dx = -dx;
            dy = -dy;
            ds = -ds;
         }
         const auto k = key(dx, dy);
         if (k == std::make_pair(0L, 0L))
            continue;   // beams at the same place: no fringe
         const int c = mod(ds);
         auto it = shift.find(k);
         if (it != shift.end())
         {
            if (it->second != c)
            {
               why = "order (" + std::to_string(dx) + ", " + std::to_string(dy) + ") shifts by two different steps";
               return false;
            }
            continue;
         }
         auto s = seen.find(c);
         if (s != seen.end())
         {
            why = "orders (" + std::to_string(dx) + ", " + std::to_string(dy) + ") and (" +
                  std::to_string(s->second.first * 1e-6) + ", " + std::to_string(s->second.second * 1e-6) +
                  ") share the shift " + std::to_string(c) + " / " + std::to_string(P);
            return false;
         }
         shift[k] = c;
         seen[c] = k;
         // Its mirror order shifts by -c.
         const int cm = mod(-ds);
         auto m = seen.find(cm);
         if (m != seen.end() && m->second != k)
         {
            why = "orders (" + std::to_string(dx) + ", " + std::to_string(dy) + ") (mirrored) and (" +
                  std::to_string(m->second.first * 1e-6) + ", " + std::to_string(m->second.second * 1e-6) +
                  ") share the shift " + std::to_string(cm) + " / " + std::to_string(P);
            return false;
         }
         seen[cm] = k;
      }
   return true;
}

} // namespace sim
