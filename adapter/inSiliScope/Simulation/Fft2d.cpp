///////////////////////////////////////////////////////////////////////////////
// FILE:          Fft2d.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Fft2d.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Fft2d.h"

#include "Parallel.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {

// Transforms processed together (interleaved) by the 2D passes.
constexpr unsigned kBlock = 16;

const double kPi = 3.14159265358979323846;

// Butterflies on B interleaved transforms: in[r] / out[r] point at element r
// (B complex each). tw[r] (r >= 1) multiplies input r first; tw == nullptr
// skips it (k = 0). The twiddles are copied into locals and the loops get
// restrict pointers, so the compiler can vectorize over b (the arithmetic
// and its order are unchanged).
inline void Twiddle(float& re, float& im, float wr, float wi)
{
   const float r = re * wr - im * wi;
   im = re * wi + im * wr;
   re = r;
}

template <bool TW>
void Radix2T(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   const float* __restrict i0 = in[0];
   const float* __restrict i1 = in[1];
   float* __restrict o0 = out[0];
   float* __restrict o1 = out[1];
   const float w1r = TW ? tw[1].real() : 1.0f, w1i = TW ? tw[1].imag() : 0.0f;
   for (unsigned b = 0; b < B; ++b)
   {
      const size_t o = 2 * static_cast<size_t>(b);
      float ar = i0[o], ai = i0[o + 1], br = i1[o], bi = i1[o + 1];
      if (TW)
         Twiddle(br, bi, w1r, w1i);
      o0[o] = ar + br;
      o0[o + 1] = ai + bi;
      o1[o] = ar - br;
      o1[o + 1] = ai - bi;
   }
}

template <bool TW>
void Radix4T(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   const float* __restrict i0 = in[0];
   const float* __restrict i1 = in[1];
   const float* __restrict i2 = in[2];
   const float* __restrict i3 = in[3];
   float* __restrict o0 = out[0];
   float* __restrict o1 = out[1];
   float* __restrict o2 = out[2];
   float* __restrict o3 = out[3];
   const float w1r = TW ? tw[1].real() : 1.0f, w1i = TW ? tw[1].imag() : 0.0f;
   const float w2r = TW ? tw[2].real() : 1.0f, w2i = TW ? tw[2].imag() : 0.0f;
   const float w3r = TW ? tw[3].real() : 1.0f, w3i = TW ? tw[3].imag() : 0.0f;
   for (unsigned b = 0; b < B; ++b)
   {
      const size_t o = 2 * static_cast<size_t>(b);
      float a0r = i0[o], a0i = i0[o + 1], a1r = i1[o], a1i = i1[o + 1];
      float a2r = i2[o], a2i = i2[o + 1], a3r = i3[o], a3i = i3[o + 1];
      if (TW)
      {
         Twiddle(a1r, a1i, w1r, w1i);
         Twiddle(a2r, a2i, w2r, w2i);
         Twiddle(a3r, a3i, w3r, w3i);
      }
      const float t0r = a0r + a2r, t0i = a0i + a2i, t1r = a0r - a2r, t1i = a0i - a2i;
      const float t2r = a1r + a3r, t2i = a1i + a3i;
      // (a1 - a3) * (-i)
      const float t3r = a1i - a3i, t3i = a3r - a1r;
      o0[o] = t0r + t2r;
      o0[o + 1] = t0i + t2i;
      o1[o] = t1r + t3r;
      o1[o + 1] = t1i + t3i;
      o2[o] = t0r - t2r;
      o2[o + 1] = t0i - t2i;
      o3[o] = t1r - t3r;
      o3[o + 1] = t1i - t3i;
   }
}

template <bool TW>
void Radix3T(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   const float c = -0.5f, s = static_cast<float>(-std::sqrt(3.0) / 2.0);
   const float* __restrict i0 = in[0];
   const float* __restrict i1 = in[1];
   const float* __restrict i2 = in[2];
   float* __restrict o0 = out[0];
   float* __restrict o1 = out[1];
   float* __restrict o2 = out[2];
   const float w1r = TW ? tw[1].real() : 1.0f, w1i = TW ? tw[1].imag() : 0.0f;
   const float w2r = TW ? tw[2].real() : 1.0f, w2i = TW ? tw[2].imag() : 0.0f;
   for (unsigned b = 0; b < B; ++b)
   {
      const size_t o = 2 * static_cast<size_t>(b);
      float a0r = i0[o], a0i = i0[o + 1], a1r = i1[o], a1i = i1[o + 1];
      float a2r = i2[o], a2i = i2[o + 1];
      if (TW)
      {
         Twiddle(a1r, a1i, w1r, w1i);
         Twiddle(a2r, a2i, w2r, w2i);
      }
      const float tr = a1r + a2r, ti = a1i + a2i;
      const float mr = a0r + c * tr, mi = a0i + c * ti;
      // i s (a1 - a2)
      const float dr = -s * (a1i - a2i), di = s * (a1r - a2r);
      o0[o] = a0r + tr;
      o0[o + 1] = a0i + ti;
      o1[o] = mr + dr;
      o1[o + 1] = mi + di;
      o2[o] = mr - dr;
      o2[o + 1] = mi - di;
   }
}

template <bool TW>
void Radix5T(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   const float c1 = static_cast<float>(std::cos(2 * kPi / 5)), s1 = static_cast<float>(-std::sin(2 * kPi / 5));
   const float c2 = static_cast<float>(std::cos(4 * kPi / 5)), s2 = static_cast<float>(-std::sin(4 * kPi / 5));
   const float* __restrict i0 = in[0];
   const float* __restrict i1 = in[1];
   const float* __restrict i2 = in[2];
   const float* __restrict i3 = in[3];
   const float* __restrict i4 = in[4];
   float* __restrict o0 = out[0];
   float* __restrict o1 = out[1];
   float* __restrict o2 = out[2];
   float* __restrict o3 = out[3];
   float* __restrict o4 = out[4];
   float wr[5] = {1, 1, 1, 1, 1}, wi[5] = {0, 0, 0, 0, 0};
   if (TW)
      for (int r = 1; r < 5; ++r)
      {
         wr[r] = tw[r].real();
         wi[r] = tw[r].imag();
      }
   for (unsigned b = 0; b < B; ++b)
   {
      const size_t o = 2 * static_cast<size_t>(b);
      float ar[5] = {i0[o], i1[o], i2[o], i3[o], i4[o]};
      float ai[5] = {i0[o + 1], i1[o + 1], i2[o + 1], i3[o + 1], i4[o + 1]};
      if (TW)
         for (int r = 1; r < 5; ++r)
            Twiddle(ar[r], ai[r], wr[r], wi[r]);
      const float t1r = ar[1] + ar[4], t1i = ai[1] + ai[4], t2r = ar[2] + ar[3], t2i = ai[2] + ai[3];
      const float t3r = ar[1] - ar[4], t3i = ai[1] - ai[4], t4r = ar[2] - ar[3], t4i = ai[2] - ai[3];
      const float m1r = ar[0] + c1 * t1r + c2 * t2r, m1i = ai[0] + c1 * t1i + c2 * t2i;
      const float m2r = ar[0] + c2 * t1r + c1 * t2r, m2i = ai[0] + c2 * t1i + c1 * t2i;
      // n1 = i (s1 t3 + s2 t4), n2 = i (s2 t3 - s1 t4)
      const float u1r = s1 * t3r + s2 * t4r, u1i = s1 * t3i + s2 * t4i;
      const float u2r = s2 * t3r - s1 * t4r, u2i = s2 * t3i - s1 * t4i;
      const float n1r = -u1i, n1i = u1r, n2r = -u2i, n2i = u2r;
      o0[o] = ar[0] + t1r + t2r;
      o0[o + 1] = ai[0] + t1i + t2i;
      o1[o] = m1r + n1r;
      o1[o + 1] = m1i + n1i;
      o4[o] = m1r - n1r;
      o4[o + 1] = m1i - n1i;
      o2[o] = m2r + n2r;
      o2[o + 1] = m2i + n2i;
      o3[o] = m2r - n2r;
      o3[o + 1] = m2i - n2i;
   }
}

void Radix2(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   if (tw) Radix2T<true>(in, out, tw, B); else Radix2T<false>(in, out, tw, B);
}
void Radix4(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   if (tw) Radix4T<true>(in, out, tw, B); else Radix4T<false>(in, out, tw, B);
}
void Radix3(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   if (tw) Radix3T<true>(in, out, tw, B); else Radix3T<false>(in, out, tw, B);
}
void Radix5(const float* const* in, float* const* out, const cfloat* tw, unsigned B)
{
   if (tw) Radix5T<true>(in, out, tw, B); else Radix5T<false>(in, out, tw, B);
}

} // namespace

FftPlan1d::FftPlan1d(unsigned n) : n_(std::max(1u, n))
{
   // Factor: 4s first, then 2, 3, 5.
   std::vector<unsigned> radices;
   unsigned m = n_;
   while (m % 4 == 0)
   {
      radices.push_back(4);
      m /= 4;
   }
   while (m % 2 == 0)
   {
      radices.push_back(2);
      m /= 2;
   }
   while (m % 3 == 0)
   {
      radices.push_back(3);
      m /= 3;
   }
   while (m % 5 == 0)
   {
      radices.push_back(5);
      m /= 5;
   }
   if (m != 1)
   {
      // Not a 2^a 3^b 5^c length: callers size with RealFft2d::FastSize.
      n_ = 1;
      return;
   }
   unsigned Ns = 1;
   for (unsigned R : radices)
   {
      Stage s;
      s.R = R;
      s.Ns = Ns;
      s.tw.resize(static_cast<size_t>(Ns) * R);
      for (unsigned k = 0; k < Ns; ++k)
         for (unsigned r = 0; r < R; ++r)
         {
            const double a = -2.0 * kPi * static_cast<double>(k) * r / (static_cast<double>(Ns) * R);
            s.tw[static_cast<size_t>(k) * R + r] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
         }
      stages_.push_back(std::move(s));
      Ns *= R;
   }
}

cfloat* FftPlan1d::Forward(cfloat* x, cfloat* y, unsigned B) const
{
   const unsigned n = n_;
   for (const Stage& s : stages_)
   {
      const unsigned R = s.R, Ns = s.Ns, stride = n / R;
      const float* in[5];
      float* out[5];
      for (unsigned j = 0; j < stride; ++j)
      {
         const unsigned k = j % Ns;
         const size_t out0 = static_cast<size_t>(j / Ns) * Ns * R + k;
         for (unsigned r = 0; r < R; ++r)
         {
            in[r] = reinterpret_cast<const float*>(x + (static_cast<size_t>(j) + static_cast<size_t>(r) * stride) * B);
            out[r] = reinterpret_cast<float*>(y + (out0 + static_cast<size_t>(r) * Ns) * B);
         }
         const cfloat* tw = k == 0 ? nullptr : &s.tw[static_cast<size_t>(k) * R];
         switch (R)
         {
         case 4: Radix4(in, out, tw, B); break;
         case 2: Radix2(in, out, tw, B); break;
         case 3: Radix3(in, out, tw, B); break;
         default: Radix5(in, out, tw, B); break;
         }
      }
      std::swap(x, y);
   }
   return x;
}

unsigned RealFft2d::FastSize(unsigned n, unsigned multiple)
{
   multiple = std::max(1u, multiple);
   for (unsigned m = std::max(n, 1u);; ++m)
   {
      if (m % multiple != 0)
         continue;
      unsigned r = m;
      for (unsigned p : {2u, 3u, 5u})
         while (r % p == 0)
            r /= p;
      if (r == 1)
         return m;
   }
}

RealFft2d::RealFft2d(unsigned nx, unsigned ny)
   : nx_(nx), ny_(ny), m_(nx / 2), rows_(nx / 2), cols_(ny)
{
   w_.resize(m_ + 1);
   for (unsigned k = 0; k <= m_; ++k)
   {
      const double a = -2.0 * kPi * k / nx;
      w_[k] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
   }
}

void RealFft2d::Forward(const float* in, unsigned inW, unsigned inH, size_t inStride, cfloat* spec) const
{
   const unsigned M = m_, W = SpecW(), B = kBlock;
   inH = std::min(inH, ny_);
   inW = std::min(inW, nx_);
   // Rows: z[n] = x[2n] + i x[2n+1], Z = FFT_M(z), X[k] = E[k] + w^k O[k].
   ParallelFor((inH + B - 1) / B, [&](unsigned blk) {
      std::vector<cfloat> a(static_cast<size_t>(M) * B), t(static_cast<size_t>(M) * B);
      const unsigned r0 = blk * B, rb = std::min(B, inH - r0);
      for (unsigned b = 0; b < rb; ++b)
      {
         const float* row = in + static_cast<size_t>(r0 + b) * inStride;
         for (unsigned n = 0; n < M; ++n)
         {
            const unsigned x0 = 2 * n;
            a[static_cast<size_t>(n) * B + b] = cfloat(x0 < inW ? row[x0] : 0.0f, x0 + 1 < inW ? row[x0 + 1] : 0.0f);
         }
      }
      for (unsigned b = rb; b < B; ++b)
         for (unsigned n = 0; n < M; ++n)
            a[static_cast<size_t>(n) * B + b] = cfloat(0.0f, 0.0f);
      const cfloat* Z = rows_.Forward(a.data(), t.data(), B);
      for (unsigned k = 0; k <= M; ++k)
      {
         const unsigned kk = k % M, km = (M - k) % M;
         const cfloat w = w_[k];
         for (unsigned b = 0; b < rb; ++b)
         {
            const cfloat zk = Z[static_cast<size_t>(kk) * B + b], zm = Z[static_cast<size_t>(km) * B + b];
            const float er = 0.5f * (zk.real() + zm.real()), ei = 0.5f * (zk.imag() - zm.imag());
            // O = (Zk - conj(Zm)) / 2i
            const float orr = 0.5f * (zk.imag() + zm.imag()), oi = -0.5f * (zk.real() - zm.real());
            spec[static_cast<size_t>(r0 + b) * W + k] =
               cfloat(er + (w.real() * orr - w.imag() * oi), ei + (w.real() * oi + w.imag() * orr));
         }
      }
   });
   std::fill(spec + static_cast<size_t>(inH) * W, spec + static_cast<size_t>(ny_) * W, cfloat(0.0f, 0.0f));
   // Columns.
   const unsigned ny = ny_;
   ParallelFor((W + B - 1) / B, [&](unsigned blk) {
      std::vector<cfloat> a(static_cast<size_t>(ny) * B), t(static_cast<size_t>(ny) * B);
      const unsigned c0 = blk * B, cb = std::min(B, W - c0);
      for (unsigned y = 0; y < ny; ++y)
      {
         const cfloat* src = spec + static_cast<size_t>(y) * W + c0;
         cfloat* dst = &a[static_cast<size_t>(y) * B];
         for (unsigned b = 0; b < cb; ++b)
            dst[b] = src[b];
         for (unsigned b = cb; b < B; ++b)
            dst[b] = cfloat(0.0f, 0.0f);
      }
      const cfloat* r = cols_.Forward(a.data(), t.data(), B);
      for (unsigned y = 0; y < ny; ++y)
      {
         cfloat* dst = spec + static_cast<size_t>(y) * W + c0;
         const cfloat* src = r + static_cast<size_t>(y) * B;
         for (unsigned b = 0; b < cb; ++b)
            dst[b] = src[b];
      }
   });
}

void RealFft2d::Inverse(cfloat* spec, float* out, unsigned row0, unsigned rows, unsigned col0, unsigned cols,
                        size_t outStride) const
{
   const unsigned M = m_, W = SpecW(), B = kBlock, ny = ny_;
   rows = std::min(rows, ny > row0 ? ny - row0 : 0u);
   cols = std::min(cols, nx_ > col0 ? nx_ - col0 : 0u);
   if (rows == 0 || cols == 0)
      return;
   // Columns (inverse = conj . forward . conj), keeping the wanted rows only.
   ParallelFor((W + B - 1) / B, [&](unsigned blk) {
      std::vector<cfloat> a(static_cast<size_t>(ny) * B), t(static_cast<size_t>(ny) * B);
      const unsigned c0 = blk * B, cb = std::min(B, W - c0);
      for (unsigned y = 0; y < ny; ++y)
      {
         const cfloat* src = spec + static_cast<size_t>(y) * W + c0;
         cfloat* dst = &a[static_cast<size_t>(y) * B];
         for (unsigned b = 0; b < cb; ++b)
            dst[b] = std::conj(src[b]);
         for (unsigned b = cb; b < B; ++b)
            dst[b] = cfloat(0.0f, 0.0f);
      }
      const cfloat* r = cols_.Forward(a.data(), t.data(), B);
      for (unsigned y = row0; y < row0 + rows; ++y)
      {
         cfloat* dst = spec + static_cast<size_t>(y) * W + c0;
         const cfloat* src = r + static_cast<size_t>(y) * B;
         for (unsigned b = 0; b < cb; ++b)
            dst[b] = std::conj(src[b]);
      }
   });
   // Rows: Z[k] = E[k] + i O[k], E = (X[k] + conj X[M-k]) / 2,
   // O = (X[k] - conj X[M-k]) / 2 conj(w^k); z = IFFT_M(Z).
   const float scale = 1.0f / (static_cast<float>(M) * static_cast<float>(ny));
   ParallelFor((rows + B - 1) / B, [&](unsigned blk) {
      std::vector<cfloat> a(static_cast<size_t>(M) * B), t(static_cast<size_t>(M) * B);
      const unsigned r0 = blk * B, rb = std::min(B, rows - r0);
      for (unsigned k = 0; k < M; ++k)
      {
         const cfloat w = w_[k];
         for (unsigned b = 0; b < rb; ++b)
         {
            const cfloat* X = spec + static_cast<size_t>(row0 + r0 + b) * W;
            const cfloat xk = X[k], xm = X[M - k];
            const float er = 0.5f * (xk.real() + xm.real()), ei = 0.5f * (xk.imag() - xm.imag());
            const float dr = 0.5f * (xk.real() - xm.real()), di = 0.5f * (xk.imag() + xm.imag());
            // O = d conj(w)
            const float orr = dr * w.real() + di * w.imag(), oi = di * w.real() - dr * w.imag();
            // conj(Z) for the inverse-by-conjugation
            a[static_cast<size_t>(k) * B + b] = cfloat(er - oi, -(ei + orr));
         }
         for (unsigned b = rb; b < B; ++b)
            a[static_cast<size_t>(k) * B + b] = cfloat(0.0f, 0.0f);
      }
      const cfloat* z = rows_.Forward(a.data(), t.data(), B);
      for (unsigned b = 0; b < rb; ++b)
      {
         float* dst = out + static_cast<size_t>(r0 + b) * outStride;
         for (unsigned c = col0; c < col0 + cols; ++c)
         {
            const cfloat v = z[static_cast<size_t>(c / 2) * B + b];
            // conj: the imaginary part flips sign
            dst[c - col0] = (c & 1u ? -v.imag() : v.real()) * scale;
         }
      }
   });
}

} // namespace sim
