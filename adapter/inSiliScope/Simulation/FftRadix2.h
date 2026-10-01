///////////////////////////////////////////////////////////////////////////////
// FILE:          FftRadix2.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Double-precision in-place radix-2 complex FFT with tabulated
//                twiddles, the same operations (so the same bits) as the
//                webSMLM/Java fft1d recurrence. Used by the Fft placement
//                (PsfGeneratorBridge.cpp) and the chirp-Z PSF (ZernikePsf.cpp).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace sim {

// The twiddle factors of Fft1d below for one (n, sign): per stage (len = 2,
// 4, ..., n) the len/2 values the in-loop recurrence cr' = cr*wr - ci*wi,
// ci' = cr*wi + ci*wr produces from (1, 0) -- the same operations, so the
// same bits -- computed once instead of once per block of every stage of
// every transform.
struct FftTwiddles
{
   int n = 0;
   std::vector<double> c, s; // stage len's values start at index len/2 - 1
};

inline FftTwiddles MakeTwiddles(int n, int sign)
{
   FftTwiddles t;
   t.n = n;
   t.c.resize(static_cast<size_t>(std::max(0, n - 1)));
   t.s.resize(t.c.size());
   for (int len = 2; len <= n; len <<= 1)
   {
      const double ang = sign * 2.0 * 3.14159265358979323846 / len;
      const double wr = std::cos(ang), wi = std::sin(ang);
      const int half = len / 2;
      double cr = 1.0, ci = 0.0;
      for (int k = 0; k < half; ++k)
      {
         t.c[static_cast<size_t>(half - 1 + k)] = cr;
         t.s[static_cast<size_t>(half - 1 + k)] = ci;
         const double nr = cr * wr - ci * wi;
         ci = cr * wi + ci * wr;
         cr = nr;
      }
   }
   return t;
}

// In-place iterative radix-2 complex FFT (contiguous); n = tw.n a power of
// two; the twiddles' sign: -1 forward, +1 inverse, both unnormalized.
inline void Fft1d(double* re, double* im, const FftTwiddles& tw)
{
   const int n = tw.n;
   for (int i = 1, j = 0; i < n; ++i)
   {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      if (i < j)
      {
         std::swap(re[i], re[j]);
         std::swap(im[i], im[j]);
      }
   }
   for (int len = 2; len <= n; len <<= 1)
   {
      const int half = len / 2;
      const double* tc = tw.c.data() + (half - 1);
      const double* ts = tw.s.data() + (half - 1);
      for (int i = 0; i < n; i += len)
      {
         double* ar = re + i;
         double* ai = im + i;
         double* br = re + i + half;
         double* bi = im + i + half;
         for (int k = 0; k < half; ++k)
         {
            const double cr = tc[k], ci = ts[k];
            const double vr = br[k] * cr - bi[k] * ci, vi = br[k] * ci + bi[k] * cr;
            br[k] = ar[k] - vr;
            bi[k] = ai[k] - vi;
            ar[k] += vr;
            ai[k] += vi;
         }
      }
   }
}

} // namespace sim
