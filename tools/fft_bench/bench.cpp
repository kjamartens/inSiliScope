// FFT library benchmark (2026-11-11): the WideField mean-field scene's forward real 2D FFT of a dye plane
// (WidefieldScene::PlaneSpectrum, sim::RealFft2d) against other BSD/MIT FFT libraries, one thread.
//
//   fft_bench [planes]       prints, per grid size and library: ms per plane (best of 3 batches), the largest error
//                            against pocketfft in double (relative to the spectrum's largest value)
//
// Input: a block of 0.75 n x 0.75 n at the origin (the scene's grid inside the padded FFT), 30 % of its cells nonzero
// (a dye plane); the rest zero, as RealFft2d::Forward(in, inW, inH, ...) sees it. Libraries: tools/fft_bench/README.md.
#include "Fft2d.h"

#include "pocketfft_hdronly.h"
extern "C" {
#include "pffft/pffft.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using cf = std::complex<float>;
using Clock = std::chrono::steady_clock;

struct Plane
{
   unsigned w = 0, h = 0;   // the block
   std::vector<float> block;
};

static std::vector<Plane> MakePlanes(unsigned n, unsigned count)
{
   std::mt19937 rng(1234);
   std::uniform_real_distribution<float> u(0.0f, 1.0f);
   std::vector<Plane> ps(count);
   for (Plane& p : ps)
   {
      p.w = (3 * n / 4) & ~1u;
      p.h = 3 * n / 4;
      p.block.assign(static_cast<size_t>(p.w) * p.h, 0.0f);
      for (float& v : p.block)
         if (u(rng) < 0.3f)
            v = 10.0f * u(rng);
   }
   return ps;
}

// Each implementation: the spectrum (ny rows of nx/2 + 1 complex) of the n x n image with the plane's block at the
// origin.
struct Impl
{
   std::string name;
   std::function<bool(unsigned n)> supports;
   std::function<void(unsigned n)> setup;
   std::function<void(const Plane& p, cf* spec)> forward;
   std::function<void()> teardown;
};

int main(int argc, char** argv)
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   const unsigned planes = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 149;
   const unsigned sizes[] = {240, 256, 360, 384, 640};

   unsigned N = 0;
   std::vector<float> img;   // n x n, zero but the block
   auto fill = [&](const Plane& p) {
      std::fill(img.begin(), img.end(), 0.0f);
      for (unsigned y = 0; y < p.h; ++y)
         std::memcpy(&img[static_cast<size_t>(y) * N], &p.block[static_cast<size_t>(y) * p.w], p.w * sizeof(float));
   };

   std::vector<Impl> impls;

   // ---- sim::RealFft2d (the engine's), with and without its zero-row skip
   sim::RealFft2d ours;
   for (int skip = 1; skip >= 0; --skip)
      impls.push_back({skip ? "RealFft2d (skip zero rows)" : "RealFft2d",
                       [](unsigned n) { return sim::RealFft2d::FastSize(n, 8) == n; },
                       [&](unsigned n) { ours = sim::RealFft2d(n, n); },
                       [&, skip](const Plane& p, cf* spec) { ours.Forward(p.block.data(), p.w, p.h, p.w, spec, skip != 0); },
                       [] {}});

   // ---- pocketfft (C++ header-only), float, one thread
   impls.push_back({"pocketfft", [](unsigned) { return true; }, [](unsigned) {},
                    [&](const Plane& p, cf* spec) {
                       fill(p);
                       const pocketfft::shape_t shape{N, N};
                       const pocketfft::stride_t si{static_cast<ptrdiff_t>(N * sizeof(float)), sizeof(float)};
                       const pocketfft::stride_t so{static_cast<ptrdiff_t>((N / 2 + 1) * sizeof(cf)), sizeof(cf)};
                       pocketfft::r2c(shape, si, so, {0, 1}, pocketfft::FORWARD, img.data(), spec, 1.0f, 1);
                    },
                    [] {}});

   // ---- pffft (SIMD): rows real (ordered), zero rows skipped, then columns complex through a transpose
   PFFFT_Setup *rowS = nullptr, *colS = nullptr;
   float *row = nullptr, *rowOut = nullptr, *work = nullptr, *col = nullptr, *colOut = nullptr;
   std::vector<cf> tmp;
   impls.push_back({"pffft (skip zero rows)",
                    [](unsigned n) { return pffft_is_valid_size(n, PFFFT_REAL) && pffft_is_valid_size(n, PFFFT_COMPLEX); },
                    [&](unsigned n) {
                       rowS = pffft_new_setup(n, PFFFT_REAL);
                       colS = pffft_new_setup(n, PFFFT_COMPLEX);
                       row = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
                       rowOut = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
                       work = static_cast<float*>(pffft_aligned_malloc(2 * n * sizeof(float)));
                       col = static_cast<float*>(pffft_aligned_malloc(2 * n * sizeof(float)));
                       colOut = static_cast<float*>(pffft_aligned_malloc(2 * n * sizeof(float)));
                       tmp.assign(static_cast<size_t>(n) * (n / 2 + 1), cf());
                    },
                    [&](const Plane& p, cf* spec) {
                       const unsigned n = N, W = n / 2 + 1;
                       // rows: tmp[y][kx]
                       for (unsigned y = 0; y < n; ++y)
                       {
                          cf* t = &tmp[static_cast<size_t>(y) * W];
                          bool any = false;
                          if (y < p.h)
                             for (unsigned x = 0; x < p.w && !any; ++x)
                                any = p.block[static_cast<size_t>(y) * p.w + x] != 0.0f;
                          if (!any)
                          {
                             std::fill(t, t + W, cf());
                             continue;
                          }
                          std::memcpy(row, &p.block[static_cast<size_t>(y) * p.w], p.w * sizeof(float));
                          std::fill(row + p.w, row + n, 0.0f);
                          pffft_transform_ordered(rowS, row, rowOut, work, PFFFT_FORWARD);
                          t[0] = cf(rowOut[0], 0.0f);
                          t[n / 2] = cf(rowOut[1], 0.0f);
                          for (unsigned k = 1; k < n / 2; ++k)
                             t[k] = cf(rowOut[2 * k], rowOut[2 * k + 1]);
                       }
                       // columns
                       for (unsigned kx = 0; kx < W; ++kx)
                       {
                          for (unsigned y = 0; y < n; ++y)
                          {
                             const cf v = tmp[static_cast<size_t>(y) * W + kx];
                             col[2 * y] = v.real();
                             col[2 * y + 1] = v.imag();
                          }
                          pffft_transform_ordered(colS, col, colOut, work, PFFFT_FORWARD);
                          for (unsigned ky = 0; ky < n; ++ky)
                             spec[static_cast<size_t>(ky) * W + kx] = cf(colOut[2 * ky], colOut[2 * ky + 1]);
                       }
                    },
                    [&] {
                       pffft_destroy_setup(rowS);
                       pffft_destroy_setup(colS);
                       for (float* q : {row, rowOut, work, col, colOut})
                          pffft_aligned_free(q);
                    }});

   std::printf("%u planes per batch, best of 3; error vs pocketfft double, relative to the largest |X|\n", planes);
   for (unsigned n : sizes)
   {
      N = n;
      img.assign(static_cast<size_t>(n) * n, 0.0f);
      const std::vector<Plane> ps = MakePlanes(n, planes);
      const size_t S = static_cast<size_t>(n) * (n / 2 + 1);
      // Reference: plane 0 in double.
      std::vector<double> imgD(static_cast<size_t>(n) * n, 0.0);
      for (unsigned y = 0; y < ps[0].h; ++y)
         for (unsigned x = 0; x < ps[0].w; ++x)
            imgD[static_cast<size_t>(y) * n + x] = ps[0].block[static_cast<size_t>(y) * ps[0].w + x];
      std::vector<std::complex<double>> ref(S);
      pocketfft::r2c({n, n}, {static_cast<ptrdiff_t>(n * sizeof(double)), sizeof(double)},
                     {static_cast<ptrdiff_t>((n / 2 + 1) * sizeof(std::complex<double>)), sizeof(std::complex<double>)},
                     {0, 1}, pocketfft::FORWARD, imgD.data(), ref.data(), 1.0, 1);
      double refMax = 0;
      for (auto& v : ref)
         refMax = std::max(refMax, std::abs(v));
      std::printf("\nn = %u%s\n", n, sim::RealFft2d::FastSize(n, 8) == n ? "" : "  (not a RealFft2d size)");
      std::vector<cf> spec(S);
      for (Impl& im : impls)
      {
         if (!im.supports(n))
         {
            std::printf("  %-28s  -\n", im.name.c_str());
            continue;
         }
         im.setup(n);
         im.forward(ps[0], spec.data());
         double err = 0;
         for (size_t i = 0; i < S; ++i)
            err = std::max(err, std::abs(std::complex<double>(spec[i]) - ref[i]));
         double best = 1e30;
         for (int rep = 0; rep < 3; ++rep)
         {
            const auto t0 = Clock::now();
            for (const Plane& p : ps)
               im.forward(p, spec.data());
            best = std::min(best, std::chrono::duration<double>(Clock::now() - t0).count());
         }
         std::printf("  %-28s  %7.3f ms/plane   err %.1e\n", im.name.c_str(), 1000.0 * best / planes, err / refMax);
         im.teardown();
      }
   }
   return 0;
}
