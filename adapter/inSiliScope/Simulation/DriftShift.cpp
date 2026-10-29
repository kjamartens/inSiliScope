///////////////////////////////////////////////////////////////////////////////
// FILE:          DriftShift.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Drift.h: the WideField/BrightField image shifts (the
//                spectrum's phase ramp, the shifted camera frame); apart from
//                Drift.cpp so the drift path needs no FFT.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Drift.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
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
