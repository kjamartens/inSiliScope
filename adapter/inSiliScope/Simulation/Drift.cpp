///////////////////////////////////////////////////////////////////////////////
// FILE:          Drift.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Drift.h.
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

std::vector<DriftNm> DriftTrajectory(long randomSeed, long frames, double frameSec, const DriftSettings& s)
{
   std::vector<DriftNm> t(static_cast<size_t>(std::max(0L, frames)));
   if (!s.On())
      return t;
   const uint32_t seed = DriftSeed(randomSeed);
   for (long f = 1; f < frames; ++f)
   {
      const DriftNm st = DriftStep(seed, f, frameSec, s);
      const DriftNm& p = t[static_cast<size_t>(f - 1)];
      DriftNm& d = t[static_cast<size_t>(f)];
      d.x = p.x + st.x;
      d.y = p.y + st.y;
      d.z = p.z + st.z;
   }
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

void ApplyShiftRamp(cfloat* S, unsigned nx, unsigned ny, double fracX, double fracY)
{
   if (fracX == 0.0 && fracY == 0.0)
      return;
   const unsigned W = nx / 2 + 1;
   std::vector<std::complex<double>> px(W), py(ny);
   for (unsigned kx = 0; kx < W; ++kx)
   {
      const double a = 2.0 * kPi * kx * fracX / nx;
      px[kx] = (kx == nx / 2) ? std::complex<double>(std::cos(a), 0.0) : std::polar(1.0, a);
   }
   for (unsigned ky = 0; ky < ny; ++ky)
   {
      const long kys = ky <= ny / 2 ? static_cast<long>(ky) : static_cast<long>(ky) - static_cast<long>(ny);
      const double a = 2.0 * kPi * kys * fracY / ny;
      py[ky] = (ky == ny / 2) ? std::complex<double>(std::cos(a), 0.0) : std::polar(1.0, a);
   }
   for (unsigned ky = 0; ky < ny; ++ky)
      for (unsigned kx = 0; kx < W; ++kx)
      {
         cfloat& v = S[static_cast<size_t>(ky) * W + kx];
         const std::complex<double> r = std::complex<double>(v) * (px[kx] * py[ky]);
         v = cfloat(static_cast<float>(r.real()), static_cast<float>(r.imag()));
      }
}

void ShiftedCamera(const RealFft2d& fft, std::vector<cfloat>& spec, double fracX, double fracY, unsigned x0,
                   unsigned y0, unsigned cw, unsigned ch, unsigned up, float scale, std::vector<float>& cam)
{
   up = std::max(1u, up);
   ApplyShiftRamp(spec.data(), fft.Nx(), fft.Ny(), fracX, fracY);
   std::vector<float> img(static_cast<size_t>(cw) * ch, 0.0f);
   fft.Inverse(spec.data(), img.data(), y0, ch, x0, cw, cw);
   const unsigned W = cw / up, H = ch / up;
   cam.resize(static_cast<size_t>(W) * H, 0.0f);
   for (unsigned Y = 0; Y < H; ++Y)
      for (unsigned X = 0; X < W; ++X)
      {
         double acc = 0.0;
         for (unsigned sy = 0; sy < up; ++sy)
         {
            const size_t row = static_cast<size_t>(Y * up + sy) * cw + X * up;
            for (unsigned sx = 0; sx < up; ++sx)
               acc += std::max(0.0f, img[row + sx]);
         }
         cam[static_cast<size_t>(Y) * W + X] += static_cast<float>(acc * scale);
      }
}

} // namespace sim
