///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMCounterRng.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Counter-based uniform/normal/Poisson/Gamma draws for the
//                camera noise chain -- a port of webSMLM's makeSimNoiseRng/
//                pcg4dInto/simNoiseGauss/simNoisePoisson/simNoiseGamma
//                (build 2026-09-21b).
//
//                A pixel's draws depend on nothing but its own address
//                (seed, frame, pixel, counter), not on how many draws earlier
//                pixels took -- so frames and pixels can be generated in any
//                order, on any number of CPU threads, or by a GPU thread, and
//                still produce the same numbers. pcg4d is pure u32
//                multiply/add/xor/shift, which HLSL's uint arithmetic
//                computes identically, and the uniform ((x>>9)+0.5)*2^-23 is
//                exact in float32, so the uniforms are bit-identical on CPU
//                and GPU. What can still differ is the float32 (GPU) vs
//                float64 (CPU) transcendental that follows (log, exp, cos):
//                a pixel whose Poisson draw sits within ~1e-7 of a boundary
//                may land one count apart.
//
//                MIRRORED LINE FOR LINE in GpuSimD3D11.cpp's HLSL source --
//                change one, change both.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sim {

inline void Pcg4d(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
{
   a = a * 1664525u + 1013904223u;
   b = b * 1664525u + 1013904223u;
   c = c * 1664525u + 1013904223u;
   d = d * 1664525u + 1013904223u;
   a += b * d;
   b += c * a;
   c += a * b;
   d += b * c;
   a ^= a >> 16;
   b ^= b >> 16;
   c ^= c >> 16;
   d ^= d >> 16;
   a += b * d;
   b += c * a;
   c += a * b;
   d += b * c;
}

// A (0,1) uniform source for one frame; Pixel(p) rewinds it to pixel p's own
// stream (counter 0).
class CounterRng
{
public:
   CounterRng(uint32_t seed, uint32_t frame) : seed_(seed), frame_(frame) {}

   void Pixel(uint32_t p)
   {
      pix_ = p;
      ctr_ = 0;
      k_ = 4;
   }

   double Uniform()
   {
      if (k_ == 4)
      {
         buf_[0] = seed_;
         buf_[1] = frame_;
         buf_[2] = pix_;
         buf_[3] = ctr_;
         Pcg4d(buf_[0], buf_[1], buf_[2], buf_[3]);
         ++ctr_;
         k_ = 0;
      }
      return ((buf_[k_++] >> 9) + 0.5) * 1.1920928955078125e-7;
   }

private:
   uint32_t seed_, frame_;
   uint32_t pix_ = 0, ctr_ = 0;
   uint32_t buf_[4] = {0, 0, 0, 0};
   int k_ = 4;
};

// Plain Box-Muller (no rejection, no cached spare): exactly two uniforms per
// normal, radius first, angle second.
inline double CounterGauss(CounterRng& u)
{
   double r = std::sqrt(-2.0 * std::log(u.Uniform()));
   return r * std::cos(6.283185307179586 * u.Uniform());
}

// Inversion (Knuth) up to mean 60, a rounded normal approximation beyond. The
// 1000-iteration cap is unreachable in practice and exists so the GPU loop
// is provably bounded; the CPU carries the same cap so both stop alike.
inline double CounterPoisson(double l, CounterRng& u)
{
   if (l > 60.0)
      return std::max(0.0, std::floor(l + std::sqrt(l) * CounterGauss(u) + 0.5));
   if (!(l > 0.0))
      return 0.0;
   const double L = std::exp(-l);
   int k = 0;
   double p = 1.0;
   do
   {
      ++k;
      p *= u.Uniform();
   } while (p > L && k < 1000);
   return static_cast<double>(k - 1);
}

// Gamma(shape k >= 1, scale 1), Marsaglia-Tsang -- the EMCCD gain register's
// output for k input electrons. Bounded for the same reason as above.
inline double CounterGamma(double k, CounterRng& u)
{
   const double d = k - 1.0 / 3.0, c = 1.0 / std::sqrt(9.0 * d);
   for (int it = 0; it < 1000; ++it)
   {
      const double x = CounterGauss(u), t = 1.0 + c * x;
      if (t <= 0.0)
         continue;
      const double v = t * t * t, w = u.Uniform();
      if (w < 1.0 - 0.0331 * x * x * x * x)
         return d * v;
      if (std::log(w) < 0.5 * x * x + d * (1.0 - v + std::log(v)))
         return d * v;
   }
   return d;
}

} // namespace sim
