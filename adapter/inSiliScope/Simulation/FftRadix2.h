///////////////////////////////////////////////////////////////////////////////
// FILE:          FftRadix2.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Double-precision in-place radix-2 complex FFT with tabulated
//                twiddles, the same operations (so the same bits) as the
//                webSMLM/Java fft1d recurrence. Used by the Fft placement
//                (PsfGeneratorBridge.cpp) and the chirp-Z PSF (ChirpZ.h,
//                ZernikePsf.cpp). Fft1dBatched runs B interleaved transforms
//                with the same per-transform operations (vectorised across
//                the batch) and can skip the first stages of a transform
//                whose input is zero except at the start.
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
// every transform. rev is the bit-reversal permutation (the pairs Fft1d's
// incremental loop swapped, tabulated).
struct FftTwiddles
{
   int n = 0;
   std::vector<double> c, s; // stage len's values start at index len/2 - 1
   std::vector<int> rev;
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
   t.rev.assign(static_cast<size_t>(std::max(1, n)), 0);
   for (int i = 1, j = 0; i < n; ++i)
   {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      t.rev[static_cast<size_t>(i)] = j;
   }
   return t;
}

// In-place iterative radix-2 complex FFT (contiguous); n = tw.n a power of
// two; the twiddles' sign: -1 forward, +1 inverse, both unnormalized.
inline void Fft1d(double* re, double* im, const FftTwiddles& tw)
{
   const int n = tw.n;
   for (int i = 1; i < n; ++i)
   {
      const int j = tw.rev[static_cast<size_t>(i)];
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

// B transforms at once, element i of transform b at re[i*B + b]: per
// transform the operations of Fft1d in its order, the batch loop innermost.
// sparseBlock > 1 (a power of two dividing n): the inputs are zero except
// at indices < n / sparseBlock, so after the bit reversal every aligned
// block of sparseBlock values holds one input at its first slot and zeros;
// the stages up to len = sparseBlock would only add exact zeros to that
// value (a + 0 = a, 0 * w = 0) and are replaced by copying it over the
// block -- the same values (up to the sign of a zero, which the squared
// modulus the callers take cannot see).
template <int B>
inline void Fft1dBatched(double* re, double* im, const FftTwiddles& tw, int sparseBlock = 1)
{
   const int n = tw.n;
   for (int i = 1; i < n; ++i)
   {
      const int j = tw.rev[static_cast<size_t>(i)];
      if (i < j)
      {
         double* ri = re + static_cast<size_t>(i) * B;
         double* rj = re + static_cast<size_t>(j) * B;
         double* ii = im + static_cast<size_t>(i) * B;
         double* ij = im + static_cast<size_t>(j) * B;
         for (int b = 0; b < B; ++b)
         {
            std::swap(ri[b], rj[b]);
            std::swap(ii[b], ij[b]);
         }
      }
   }
   int len = 2;
   if (sparseBlock > 1)
   {
      for (int g = 0; g < n; g += sparseBlock)
      {
         const double* r0 = re + static_cast<size_t>(g) * B;
         const double* i0 = im + static_cast<size_t>(g) * B;
         for (int t = 1; t < sparseBlock; ++t)
         {
            double* rt = re + static_cast<size_t>(g + t) * B;
            double* it = im + static_cast<size_t>(g + t) * B;
            for (int b = 0; b < B; ++b)
            {
               rt[b] = r0[b];
               it[b] = i0[b];
            }
         }
      }
      len = 2 * sparseBlock;
   }
   for (; len <= n; len <<= 1)
   {
      const int half = len / 2;
      const double* tc = tw.c.data() + (half - 1);
      const double* ts = tw.s.data() + (half - 1);
      for (int i = 0; i < n; i += len)
      {
         double* ar = re + static_cast<size_t>(i) * B;
         double* ai = im + static_cast<size_t>(i) * B;
         double* br = re + static_cast<size_t>(i + half) * B;
         double* bi = im + static_cast<size_t>(i + half) * B;
         for (int k = 0; k < half; ++k)
         {
            const double cr = tc[k], ci = ts[k];
            double* ark = ar + static_cast<size_t>(k) * B;
            double* aik = ai + static_cast<size_t>(k) * B;
            double* brk = br + static_cast<size_t>(k) * B;
            double* bik = bi + static_cast<size_t>(k) * B;
            for (int b = 0; b < B; ++b)
            {
               const double vr = brk[b] * cr - bik[b] * ci, vi = brk[b] * ci + bik[b] * cr;
               brk[b] = ark[b] - vr;
               bik[b] = aik[b] - vi;
               ark[b] += vr;
               aik[b] += vi;
            }
         }
      }
   }
}

} // namespace sim
