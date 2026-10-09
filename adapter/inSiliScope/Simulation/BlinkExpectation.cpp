// BlinkExpectation.cpp -- see BlinkExpectation.h.
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

#include "BlinkExpectation.h"
#include "DyeLibrary.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim {

namespace {

// The chain's states, and the ON-time integral as a sixth "state".
enum { S_INIT = 0, S_DARK = 1, S_ON = 2, S_OFF = 3, S_BLEACHED = 4, S_INT = 5, NS = 6 };
// A state left at once (a mean duration <= 0): a rate far above any other.
constexpr double kInstant = 1e12;
constexpr double kOnCap = 20.0;   // PERSIST_ON_CAP
constexpr double kBinSec = 1.0;   // PERSIST_BIN_SEC

using Mat = double[NS][NS];

void MatMul(const Mat& a, const Mat& b, Mat& out)
{
   for (int i = 0; i < NS; ++i)
      for (int j = 0; j < NS; ++j)
      {
         double s = 0.0;
         for (int k = 0; k < NS; ++k)
            s += a[i][k] * b[k][j];
         out[i][j] = s;
      }
}

// exp(Q T) by scaling and squaring of a degree-16 Taylor sum.
void Expm(const Mat& Q, double T, Mat& E)
{
   Mat A;
   double norm = 0.0;
   for (int i = 0; i < NS; ++i)
   {
      double r = 0.0;
      for (int j = 0; j < NS; ++j)
      {
         A[i][j] = Q[i][j] * T;
         r += std::fabs(A[i][j]);
      }
      norm = std::max(norm, r);
   }
   int s = 0;
   double scale = 1.0;
   while (norm > 0.5)
   {
      norm *= 0.5;
      scale *= 0.5;
      ++s;
   }
   Mat term, next;
   for (int i = 0; i < NS; ++i)
      for (int j = 0; j < NS; ++j)
      {
         A[i][j] *= scale;
         term[i][j] = i == j ? 1.0 : 0.0;
         E[i][j] = term[i][j];
      }
   for (int k = 1; k <= 16; ++k)
   {
      MatMul(term, A, next);
      for (int i = 0; i < NS; ++i)
         for (int j = 0; j < NS; ++j)
         {
            term[i][j] = next[i][j] / k;
            E[i][j] += term[i][j];
         }
   }
   for (int q = 0; q < s; ++q)
   {
      MatMul(E, E, next);
      for (int i = 0; i < NS; ++i)
         for (int j = 0; j < NS; ++j)
            E[i][j] = next[i][j];
   }
}

// v = v exp(Q T) (row vector).
void Propagate(double v[NS], const Mat& Q, double T)
{
   if (!(T > 0.0))
      return;
   Mat E;
   Expm(Q, T, E);
   double w[NS];
   for (int j = 0; j < NS; ++j)
   {
      double s = 0.0;
      for (int i = 0; i < NS; ++i)
         s += v[i] * E[i][j];
      w[j] = s;
   }
   for (int j = 0; j < NS; ++j)
      v[j] = w[j];
}

double RateOf(double meanSec)
{
   return meanSec > 0.0 ? 1.0 / meanSec : kInstant;
}

void Generator(const BlinkKineticsSegment& k, Mat& Q)
{
   for (int i = 0; i < NS; ++i)
      for (int j = 0; j < NS; ++j)
         Q[i][j] = 0.0;
   const double p = std::min(1.0, std::max(0.01, k.bleachProb));
   const double rOn = RateOf(k.onSec);
   Q[S_INIT][S_DARK] = k.initialOnSec > 0.0 ? 1.0 / k.initialOnSec : 0.0;
   Q[S_DARK][S_ON] = std::max(0.0, k.activationRatePerSec);
   Q[S_ON][S_BLEACHED] = p * rOn;
   Q[S_ON][S_OFF] = (1.0 - p) * rOn;
   Q[S_OFF][S_ON] = RateOf(k.offSec);
   for (int i = 0; i < S_INT; ++i)
   {
      double out = 0.0;
      for (int j = 0; j < S_INT; ++j)
         if (j != i)
            out += Q[i][j];
      Q[i][i] = -out;
   }
   Q[S_ON][S_INT] = 1.0;   // d(integral)/dt = P(ON)
}

double MarkovOnSeconds(int mode, const std::vector<BlinkKineticsSegment>& segs, double t0, double t1)
{
   double v[NS] = { 0, 0, 0, 0, 0, 0 };
   const bool initialOn = mode == MODE_DSTORM && segs[0].initialOnSec > 0.0;
   v[initialOn ? S_INIT : S_DARK] = 1.0;
   for (size_t s = 0; s < segs.size(); ++s)
   {
      const double a = segs[s].tStart;
      const double b = s + 1 < segs.size() ? segs[s + 1].tStart : std::numeric_limits<double>::infinity();
      if (!(a < t1))
         break;
      // An initial ON whose segment has none ends at once (HazardWalk::AfterScale).
      if (v[S_INIT] != 0.0 && !(segs[s].initialOnSec > 0.0))
      {
         v[S_DARK] += v[S_INIT];
         v[S_INIT] = 0.0;
      }
      Mat Q;
      Generator(segs[s], Q);
      if (a < t0)
      {
         Propagate(v, Q, std::min(b, t0) - a);
         v[S_INT] = 0.0;
      }
      const double lo = std::max(a, t0), hi = std::min(b, t1);
      if (hi > lo)
         Propagate(v, Q, hi - lo);
   }
   return v[S_INT];
}

// The segment holding clock t (KineticsHistory::SegmentAt).
size_t SegmentAt(const std::vector<BlinkKineticsSegment>& segs, double t)
{
   size_t lo = 0, hi = segs.size();
   while (hi - lo > 1)
   {
      const size_t mid = (lo + hi) / 2;
      if (segs[mid].tStart <= t)
         lo = mid;
      else
         hi = mid;
   }
   return lo;
}

// Expected overlap of [a, a + D) with [t0, t1) for a uniform in [b, b + 1),
// D = min(Exp(tau), cap), integrated over a: the integral over a of
// tau (exp(-(L - a) / tau) - exp(-(U - a) / tau)), L = max(t0, a),
// U = min(t1, a + cap), where U > L.
double PaintBinOverlap(double b, double tau, double cap, double t0, double t1)
{
   double pts[6] = { b, b + kBinSec, t0, t1, t0 - cap, t1 - cap };
   std::sort(pts + 2, pts + 6);
   double cuts[6];
   int n = 0;
   cuts[n++] = b;
   for (int i = 2; i < 6; ++i)
      if (pts[i] > b && pts[i] < b + kBinSec)
         cuts[n++] = pts[i];
   cuts[n++] = b + kBinSec;
   double sum = 0.0;
   for (int i = 0; i + 1 < n; ++i)
   {
      const double al = cuts[i], be = cuts[i + 1];
      if (!(be > al))
         continue;
      const double m = 0.5 * (al + be);
      const bool lIsA = m >= t0, uIsCap = m + cap <= t1;
      const double Lm = lIsA ? m : t0, Um = uIsCap ? m + cap : t1;
      if (!(Um > Lm))
         continue;
      const double t1Term = lIsA ? tau * (be - al)
                                 : tau * tau * (std::exp(-(t0 - be) / tau) - std::exp(-(t0 - al) / tau));
      const double t2Term = uIsCap ? tau * std::exp(-cap / tau) * (be - al)
                                   : tau * tau * (std::exp(-(t1 - be) / tau) - std::exp(-(t1 - al) / tau));
      sum += t1Term - t2Term;
   }
   return sum;
}

double PaintOnSeconds(const std::vector<BlinkKineticsSegment>& segs, double t0, double t1)
{
   double maxOn = 0.0;
   for (const BlinkKineticsSegment& k : segs)
      maxOn = std::max(maxOn, kOnCap * k.onSec);
   const long b0 = std::max(0L, static_cast<long>(std::floor((t0 - maxOn) / kBinSec)) - 1);
   const long b1 = static_cast<long>(std::floor(t1 / kBinSec));
   double sum = 0.0;
   for (long b = b0; b <= b1; ++b)
   {
      const BlinkKineticsSegment& k = segs[SegmentAt(segs, b * kBinSec)];
      if (!(k.activationRatePerSec > 0.0) || !(k.onSec > 0.0))
         continue;
      sum += k.activationRatePerSec * PaintBinOverlap(b * kBinSec, k.onSec, kOnCap * k.onSec, t0, t1);
   }
   return sum;
}

} // namespace

double ExpectedBlinkOnSeconds(int mode, const std::vector<BlinkKineticsSegment>& segs, double t0, double t1)
{
   if (segs.empty() || !(t1 > t0) || mode == MODE_WIDEFIELD)
      return 0.0;
   if (mode == MODE_DNA_PAINT)
      return PaintOnSeconds(segs, t0, t1);
   return MarkovOnSeconds(mode, segs, t0, t1);
}

} // namespace sim
