///////////////////////////////////////////////////////////////////////////////
// FILE:          Fft2d.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Fft2d.h.
//
// LICENSE:       BSD (see license.txt)

#include "Fft2d.h"

#include <cmath>
#include <utility>

namespace sim {

unsigned Fft2d::NextPow2(unsigned v)
{
   unsigned n = 1;
   while (n < v)
      n <<= 1;
   return n;
}

Fft2d::Fft2d(unsigned n) : n_(NextPow2(n < 1 ? 1 : n))
{
   while ((1u << log2_) < n_)
      ++log2_;
   tw_.resize(n_ / 2 > 0 ? n_ / 2 : 1);
   const double pi = 3.14159265358979323846;
   for (unsigned k = 0; k < n_ / 2; ++k)
   {
      const double a = -2.0 * pi * k / n_;
      tw_[k] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
   }
   rev_.resize(n_);
   for (unsigned i = 0; i < n_; ++i)
   {
      unsigned r = 0;
      for (unsigned b = 0; b < log2_; ++b)
         r |= ((i >> b) & 1u) << (log2_ - 1 - b);
      rev_[i] = r;
   }
}

void Fft2d::Rows(cfloat* d, bool inverse) const
{
   const unsigned n = n_;
   for (unsigned row = 0; row < n; ++row)
   {
      float* x = reinterpret_cast<float*>(d + static_cast<size_t>(row) * n);
      for (unsigned i = 0; i < n; ++i)
      {
         const unsigned j = rev_[i];
         if (j > i)
         {
            std::swap(x[2 * i], x[2 * j]);
            std::swap(x[2 * i + 1], x[2 * j + 1]);
         }
      }
      for (unsigned len = 2; len <= n; len <<= 1)
      {
         const unsigned half = len / 2, step = n / len;
         for (unsigned s = 0; s < n; s += len)
            for (unsigned k = 0; k < half; ++k)
            {
               const cfloat w = tw_[k * step];
               const float wr = w.real(), wi = inverse ? -w.imag() : w.imag();
               float* a = x + 2 * (s + k);
               float* b = x + 2 * (s + k + half);
               // Written out: std::complex operator* may call a NaN-checking
               // helper and is slower.
               const float tr = b[0] * wr - b[1] * wi;
               const float ti = b[0] * wi + b[1] * wr;
               b[0] = a[0] - tr;
               b[1] = a[1] - ti;
               a[0] = a[0] + tr;
               a[1] = a[1] + ti;
            }
      }
   }
}

void Fft2d::Transpose(cfloat* d) const
{
   const unsigned n = n_;
   for (unsigned y = 0; y < n; ++y)
      for (unsigned x = y + 1; x < n; ++x)
         std::swap(d[static_cast<size_t>(y) * n + x], d[static_cast<size_t>(x) * n + y]);
}

void Fft2d::Forward(cfloat* d) const
{
   Rows(d, false);
   Transpose(d);
   Rows(d, false);
   Transpose(d);
}

void Fft2d::Inverse(cfloat* d) const
{
   Rows(d, true);
   Transpose(d);
   Rows(d, true);
   Transpose(d);
   const float s = 1.0f / static_cast<float>(Size());
   float* f = reinterpret_cast<float*>(d);
   for (size_t i = 0; i < 2 * Size(); ++i)
      f[i] *= s;
}

} // namespace sim
