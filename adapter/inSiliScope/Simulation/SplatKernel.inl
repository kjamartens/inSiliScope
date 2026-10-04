///////////////////////////////////////////////////////////////////////////////
// FILE:          SplatKernel.inl
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Body of the splat row kernel (see SplatKernel.h), included
//                by SplatSse2.cpp and SplatAvx2.cpp with ISC_SPLAT_NS set to
//                their namespace. Everything here has internal linkage
//                except the one SplatRows entry point. The interior loops are
//                plain loops over independent lanes on purpose: the compiler
//                vectorises them (measured faster than hand-written
//                intrinsics on MSVC), and without contraction every lane is
//                the scalar float -> double, multiply, add sequence.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "SplatKernel.h"

#ifndef ISC_SPLAT_NS
#error "define ISC_SPLAT_NS before including SplatKernel.inl"
#endif

#if defined(_MSC_VER)
#define ISC_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define ISC_RESTRICT __restrict__
#else
#define ISC_RESTRICT
#endif

namespace sim {
namespace ISC_SPLAT_NS {
namespace {

inline int MinI(int a, int b) { return a < b ? a : b; }
inline int MaxI(int a, int b) { return a > b ? a : b; }
// floor(a / b) and ceil(a / b) for b > 0, any sign of a.
inline int FloorDivI(int a, int b)
{
   const int q = a / b;
   return (a % b != 0 && a < 0) ? q - 1 : q;
}
inline int CeilDivI(int a, int b) { return -FloorDivI(-a, b); }

// One camera pixel's interpolated block-sum read: taps j in [jLo, jHi) and i
// in [iLo, iHi) (the taps inside the block-sum array), in index order -- the
// order and the operations of the original all-taps loop, which skipped the
// same taps.
inline double PixelTaps(const float* B, int bw, int r0, int c0, int jLo, int jHi, int iLo, int iHi,
                        const double* wx, const double* wy)
{
   double sum = 0.0;
   for (int j = jLo; j < jHi; ++j)
   {
      const float* brow = B + static_cast<size_t>(r0 + j) * bw;
      double row = 0.0;
      for (int i = iLo; i < iHi; ++i)
         row += wx[i] * brow[c0 + i];
      sum += wy[j] * row;
   }
   return sum;
}

template <int NT>
void Rows(const SplatArgs& a)
{
   const int os = a.os, off = os - 1, bw = a.bw, qw = a.qw, camRad = a.camRad;
   const int dyLo = MaxI(-camRad, a.yLo - a.y0), dyHi = MinI(camRad, a.yHi - 1 - a.y0);
   const int dxLo = MaxI(-camRad, -a.x0), dxHi = MinI(camRad, static_cast<int>(a.width) - 1 - a.x0);
   const int base = a.bx + off;   // tap column of pixel dx, tap i: base + dx*os + i

   // Interior pixels [dxA, dxB]: every tap column base + dx*os + i, i in
   // [0, NT), lies inside [0, bw). The others take the scalar path.
   int dxA = dxHi + 1, dxB = dxHi;
   if (a.P && qw > 0)
   {
      dxA = MaxI(dxLo, CeilDivI(-base, os));
      dxB = MinI(dxHi, FloorDivI(bw - NT - base, os));
   }
   // Tap i reads polySums phase `phase[i]` at column q0[i] + dx.
   int phase[NT], q0[NT];
   for (int i = 0; i < NT; ++i)
   {
      const int b = base + i, fq = FloorDivI(b, os);
      phase[i] = b - fq * os;
      q0[i] = fq;
   }

   constexpr int kChunk = 256;
   double rowv[kChunk], sumv[kChunk];
   for (int dy = dyLo; dy <= dyHi; ++dy)
   {
      const int r0 = a.by + dy * os + off;
      const int jLo = MaxI(0, -r0), jHi = MinI(NT, bw - r0);
      float* rowOut = a.img + static_cast<size_t>(a.y0 + dy) * a.width;
      // Left edge (and every pixel when there is no polyphase plane).
      for (int dx = dxLo; dx < dxA && dx <= dxHi; ++dx)
      {
         const int c0 = base + dx * os;
         const int iLo = MaxI(0, -c0), iHi = MinI(NT, bw - c0);
         rowOut[a.x0 + dx] += static_cast<float>(a.photons * PixelTaps(a.B, bw, r0, c0, jLo, jHi, iLo, iHi, a.wx, a.wy));
      }
      // Interior: per pixel k the same `row = 0; row += wx[i]*B; sum += wy[j]*row;
      // out += float(photons*sum)` sequence as PixelTaps, on lanes.
      for (int dx0 = dxA; dx0 <= dxB; dx0 += kChunk)
      {
         const int n = MinI(kChunk, dxB - dx0 + 1);
         for (int k = 0; k < n; ++k)
            sumv[k] = 0.0;
         for (int j = jLo; j < jHi; ++j)
         {
            for (int k = 0; k < n; ++k)
               rowv[k] = 0.0;
            for (int i = 0; i < NT; ++i)
            {
               const float* ISC_RESTRICT src =
                  a.P + (static_cast<size_t>(r0 + j) * os + phase[i]) * qw + (q0[i] + dx0);
               double* ISC_RESTRICT rv = rowv;
               const double w = a.wx[i];
               for (int k = 0; k < n; ++k)
                  rv[k] += w * src[k];
            }
            const double w = a.wy[j];
            for (int k = 0; k < n; ++k)
               sumv[k] += w * rowv[k];
         }
         float* ISC_RESTRICT o = rowOut + a.x0 + dx0;
         const double ph = a.photons;
         for (int k = 0; k < n; ++k)
            o[k] += static_cast<float>(ph * sumv[k]);
      }
      // Right edge.
      for (int dx = MaxI(dxB + 1, dxA); dx <= dxHi; ++dx)
      {
         const int c0 = base + dx * os;
         const int iLo = MaxI(0, -c0), iHi = MinI(NT, bw - c0);
         rowOut[a.x0 + dx] += static_cast<float>(a.photons * PixelTaps(a.B, bw, r0, c0, jLo, jHi, iLo, iHi, a.wx, a.wy));
      }
   }
}

} // namespace

void SplatRows(const SplatArgs& a)
{
   switch (a.nTaps)
   {
   case 1: Rows<1>(a); break;
   case 2: Rows<2>(a); break;
   default: Rows<4>(a); break;
   }
}

} // namespace ISC_SPLAT_NS
} // namespace sim

#undef ISC_RESTRICT
