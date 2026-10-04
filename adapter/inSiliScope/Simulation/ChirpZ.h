///////////////////////////////////////////////////////////////////////////////
// FILE:          ChirpZ.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Bluestein chirp-Z transform, out[p] = sum_m in[m] exp(i k_m
//                x_p), k_m = k0 + m dk, x_p = x0 + p dx, m < M, p < P (webSMLM's
//                psfCzt1d; the GibsonLanniZernike PSF's pupil -> image lines),
//                with every input-independent factor tabulated. Apply<B> runs
//                B lines at once on interleaved scratch: each line performs
//                exactly the operations of the one-line transform, in the
//                same order (the batch loop is innermost), so the results
//                are the same bits while the compiler vectorises across the
//                batch (ctest zernike_psf compares with the one-line code).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "FftRadix2.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sim {

struct CztPlan
{
   int M = 0, P = 0, L = 0;
   int sparseBlock = 1;            // the forward FFT's input is zero beyond M: blocks of L/pow2ceil(M)
   double invL = 1.0;              // 1/L, exact (L a power of two)
   FftTwiddles fwd, inv;
   std::vector<double> preC, preS; // exp(i*(m*dk*x0 + theta*m*m/2))
   std::vector<double> gRe, gIm;   // FFT of the filter exp(-i*theta*n*n/2)
   std::vector<double> c1, s1;     // exp(i*theta*p*p/2)
   std::vector<double> c2, s2;     // exp(i*k0*(x0+p*dx))

   CztPlan() = default;
   CztPlan(int M_, double dk, double k0, int P_, double dx, double x0) : M(M_), P(P_)
   {
      const double theta = dk * dx;
      L = 1;
      while (L < M + P - 1)
         L <<= 1;
      invL = 1.0 / L;
      int mp = 1;
      while (mp < M)
         mp <<= 1;
      sparseBlock = L / mp;
      fwd = MakeTwiddles(L, -1);
      inv = MakeTwiddles(L, 1);
      preC.resize(static_cast<size_t>(M));
      preS.resize(static_cast<size_t>(M));
      for (int m = 0; m < M; ++m)
      {
         const double ang = m * dk * x0 + theta * m * m / 2.0;
         preC[static_cast<size_t>(m)] = std::cos(ang);
         preS[static_cast<size_t>(m)] = std::sin(ang);
      }
      gRe.assign(static_cast<size_t>(L), 0.0);
      gIm.assign(static_cast<size_t>(L), 0.0);
      for (int n = -(M - 1); n < P; ++n)
      {
         const double ang = -theta * n * n / 2.0;
         const int idx = n >= 0 ? n : L + n;
         gRe[static_cast<size_t>(idx)] = std::cos(ang);
         gIm[static_cast<size_t>(idx)] = std::sin(ang);
      }
      Fft1d(gRe.data(), gIm.data(), fwd);
      c1.resize(static_cast<size_t>(P));
      s1.resize(c1.size());
      c2.resize(c1.size());
      s2.resize(c1.size());
      for (int p = 0; p < P; ++p)
      {
         const double a1 = theta * p * p / 2.0;
         c1[static_cast<size_t>(p)] = std::cos(a1);
         s1[static_cast<size_t>(p)] = std::sin(a1);
         const double a2 = k0 * (x0 + p * dx);
         c2[static_cast<size_t>(p)] = std::cos(a2);
         s2[static_cast<size_t>(p)] = std::sin(a2);
      }
   }

   // Scratch doubles per batch of B lines (aRe and aIm each).
   size_t ScratchPerBatch(int B) const { return static_cast<size_t>(L) * B; }

   // B lines at once: inRe[b]/inIm[b] (M values each), outRe[b]/outIm[b] (P
   // values each); aRe/aIm: scratch of L*B doubles each.
   template <int B>
   void Apply(const double* const* inRe, const double* const* inIm, double* const* outRe, double* const* outIm,
              double* aRe, double* aIm) const
   {
      std::fill(aRe, aRe + static_cast<size_t>(L) * B, 0.0);
      std::fill(aIm, aIm + static_cast<size_t>(L) * B, 0.0);
      for (int m = 0; m < M; ++m)
      {
         const double cr = preC[static_cast<size_t>(m)], ci = preS[static_cast<size_t>(m)];
         double* ar = aRe + static_cast<size_t>(m) * B;
         double* ai = aIm + static_cast<size_t>(m) * B;
         for (int b = 0; b < B; ++b)
         {
            const double re = inRe[b][m], im = inIm[b][m];
            ar[b] = re * cr - im * ci;
            ai[b] = re * ci + im * cr;
         }
      }
      // Input zero beyond M: the first stages only spread each block's one
      // value (Fft1dBatched's sparseBlock), the same bits as computing them.
      Fft1dBatched<B>(aRe, aIm, fwd, sparseBlock);
      for (int i = 0; i < L; ++i)
      {
         const double gr = gRe[static_cast<size_t>(i)], gi = gIm[static_cast<size_t>(i)];
         double* ar = aRe + static_cast<size_t>(i) * B;
         double* ai = aIm + static_cast<size_t>(i) * B;
         for (int b = 0; b < B; ++b)
         {
            const double re = ar[b] * gr - ai[b] * gi;
            const double im = ar[b] * gi + ai[b] * gr;
            ar[b] = re;
            ai[b] = im;
         }
      }
      Fft1dBatched<B>(aRe, aIm, inv, 1);
      for (int p = 0; p < P; ++p)
      {
         const double cc1 = c1[static_cast<size_t>(p)], ss1 = s1[static_cast<size_t>(p)];
         const double cc2 = c2[static_cast<size_t>(p)], ss2 = s2[static_cast<size_t>(p)];
         const double* ar = aRe + static_cast<size_t>(p) * B;
         const double* ai = aIm + static_cast<size_t>(p) * B;
         for (int b = 0; b < B; ++b)
         {
            // x / L == x * (1/L) exactly: L is a power of two.
            const double convRe = ar[b] * invL, convIm = ai[b] * invL;
            const double sRe = convRe * cc1 - convIm * ss1, sIm = convRe * ss1 + convIm * cc1;
            outRe[b][p] = sRe * cc2 - sIm * ss2;
            outIm[b][p] = sRe * ss2 + sIm * cc2;
         }
      }
   }
};

} // namespace sim
