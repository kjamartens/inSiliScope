///////////////////////////////////////////////////////////////////////////////
// FILE:          Fft2d.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Real 2D FFT (r2c / c2r) of an nx x ny image, any sizes
//                2^a 3^b 5^c (nx even): a mixed-radix (4, 2, 3, 5) Stockham
//                complex FFT, batched over blocks of rows / columns so the
//                innermost loop runs over contiguous transforms. Rows use the
//                half-length complex trick. Every row and column is an
//                independent transform, so the result does not depend on the
//                thread count. No third-party dependency (it stays small in
//                the viewer's WASM) and the same operation order native and
//                WASM.
//
//                Spectrum layout: ny rows of SpecW() = nx/2 + 1 complex
//                (kx = 0..nx/2), row ky = 0..ny-1; the other half is the
//                Hermitian mirror.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace sim {

using cfloat = std::complex<float>;

// Batched 1D complex FFT of length n = 2^a 3^b 5^c. Element i of transform b
// is at x[i * B + b].
class FftPlan1d
{
public:
   explicit FftPlan1d(unsigned n = 1);
   unsigned N() const { return n_; }
   // Forward (exp(-2 pi i ...)) transform of B interleaved sequences; x and
   // work are n * B long. Returns the buffer holding the result (x or work).
   cfloat* Forward(cfloat* x, cfloat* work, unsigned B) const;

private:
   struct Stage
   {
      unsigned R, Ns;
      std::vector<cfloat> tw; // tw[k * R + r] = exp(-2 pi i k r / (Ns R))
   };
   unsigned n_ = 1;
   std::vector<Stage> stages_;
};

class RealFft2d
{
public:
   RealFft2d() = default;
   RealFft2d(unsigned nx, unsigned ny); // nx even; both 2^a 3^b 5^c
   unsigned Nx() const { return nx_; }
   unsigned Ny() const { return ny_; }
   unsigned SpecW() const { return nx_ / 2 + 1; }
   size_t SpecSize() const { return static_cast<size_t>(SpecW()) * ny_; }
   bool Valid() const { return nx_ > 0; }

   // Spectrum of the nx x ny image that is `in` (inW x inH, row stride
   // inStride) at the origin and zero elsewhere. spec: SpecSize().
   // skipZeroRows: rows of `in` that are all zero are not transformed (their
   // spectrum rows set to zero): the same values, faster for a sparse image
   // (the binned blinks, BinnedBlinks.h); the sign of an exact zero may differ.
   void Forward(const float* in, unsigned inW, unsigned inH, size_t inStride, cfloat* spec,
                bool skipZeroRows = false) const;
   // Inverse, normalised (1/(nx ny)): rows [row0, row0 + rows) of the image,
   // columns [col0, col0 + cols), into out (row stride outStride). spec is
   // destroyed.
   void Inverse(cfloat* spec, float* out, unsigned row0, unsigned rows, unsigned col0, unsigned cols,
                size_t outStride) const;

   // Smallest m >= n with m = 2^a 3^b 5^c and m % multiple == 0; no5: 2^a 3^b
   // only (the radix-5 stages are several times slower per point).
   static unsigned FastSize(unsigned n, unsigned multiple = 2, bool no5 = false);

private:
   unsigned nx_ = 0, ny_ = 0, m_ = 0; // m = nx / 2
   FftPlan1d rows_, cols_;
   std::vector<cfloat> w_;            // exp(-2 pi i k / nx), k <= m
};

} // namespace sim
