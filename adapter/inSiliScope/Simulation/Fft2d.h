///////////////////////////////////////////////////////////////////////////////
// FILE:          Fft2d.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   In-place N x N complex<float> FFT, N = 2^k: iterative
//                radix-2 with precomputed twiddles, rows then columns via a
//                transpose. No third-party dependency (it stays small in the
//                viewer's WASM) and the same operation order native and WASM.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace sim {

using cfloat = std::complex<float>;

class Fft2d
{
public:
   explicit Fft2d(unsigned n = 1); // n is rounded up to a power of two
   unsigned N() const { return n_; }
   size_t Size() const { return static_cast<size_t>(n_) * n_; }
   void Forward(cfloat* d) const;
   void Inverse(cfloat* d) const; // normalised (1/N^2)

   // Z = F(a + i b) of two real images a, b: their spectra are
   // A[k] = (Z[k] + conj(Z[-k])) / 2 and B[k] = (Z[k] - conj(Z[-k])) / 2i.
   // Index of -k (row-major k = ky*N + kx).
   size_t Neg(size_t k) const
   {
      const size_t kx = k % n_, ky = k / n_;
      return ((n_ - ky) & (n_ - 1)) * n_ + ((n_ - kx) & (n_ - 1));
   }

   static unsigned NextPow2(unsigned v);

private:
   void Rows(cfloat* d, bool inverse) const;
   void Transpose(cfloat* d) const;
   unsigned n_ = 1, log2_ = 0;
   std::vector<cfloat> tw_;     // exp(-2 pi i k / N), k < N/2
   std::vector<unsigned> rev_;  // bit reversal
};

} // namespace sim
