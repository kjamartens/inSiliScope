// BinnedBlinks.cpp -- see BinnedBlinks.h.
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

#include "BinnedBlinks.h"
#include "Fft2d.h"
#include "WidefieldRender.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>

namespace sim {

// The kernel spectra of one (kernel, upscale, grid): one per PSF z plane,
// made on first use (thread-safe), kept for the object's life.
struct BinnedKernelSpectra
{
   unsigned nx = 0, ny = 0;
   int u = 1, R = 1, nz = 1;
   RealFft2d fft;
   std::unique_ptr<WidefieldPsf> psf; // null: the Gaussian below
   std::vector<float> gauss;          // (2R+1)^2 cells, sum 1
   std::unique_ptr<std::once_flag[]> once;
   std::vector<std::vector<cfloat>> spec;

   const std::vector<cfloat>& Spectrum(int p)
   {
      std::call_once(once[static_cast<size_t>(p)], [&] {
         std::vector<float> k;
         if (psf)
            psf->Kernel(p, R, k);
         else
            k = gauss;
         // The kernel wrapped around the origin of the nx x ny grid.
         const int D = 2 * R + 1;
         std::vector<float> wrapped(static_cast<size_t>(nx) * ny, 0.0f);
         for (int dy = -R; dy <= R; ++dy)
         {
            const unsigned y = static_cast<unsigned>((dy + static_cast<int>(ny)) % static_cast<int>(ny));
            for (int dx = -R; dx <= R; ++dx)
            {
               const unsigned x = static_cast<unsigned>((dx + static_cast<int>(nx)) % static_cast<int>(nx));
               wrapped[static_cast<size_t>(y) * nx + x] += k[static_cast<size_t>(dy + R) * D + (dx + R)];
            }
         }
         std::vector<cfloat>& s = spec[static_cast<size_t>(p)];
         s.resize(fft.SpecSize());
         fft.Forward(wrapped.data(), nx, ny, nx, s.data());
      });
      return spec[static_cast<size_t>(p)];
   }
};

namespace {

struct SpectraKey
{
   uint64_t planes = 0, splat = 0;
   int u = 0, R = 0;
   unsigned W = 0, H = 0;
   double sigma = 0.0;
   bool operator==(const SpectraKey& o) const
   {
      return planes == o.planes && splat == o.splat && u == o.u && R == o.R && W == o.W && H == o.H &&
             sigma == o.sigma;
   }
};

// The last two grids' spectra (an SR movie and a live session, or two dyes).
std::shared_ptr<BinnedKernelSpectra> AcquireSpectra(const SpectraKey& key,
                                                    const std::function<std::shared_ptr<BinnedKernelSpectra>()>& make)
{
   static std::mutex mutex;
   static std::vector<std::pair<SpectraKey, std::shared_ptr<BinnedKernelSpectra>>> slots;
   std::lock_guard<std::mutex> lock(mutex);
   for (size_t i = 0; i < slots.size(); ++i)
      if (slots[i].first == key)
      {
         auto hit = slots[i];
         slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(i));
         slots.insert(slots.begin(), hit);
         return hit.second;
      }
   auto made = make();
   slots.insert(slots.begin(), {key, made});
   if (slots.size() > 2)
      slots.resize(2);
   return made;
}

} // namespace

bool BinnedBlinkRenderer::Setup(const PsfKernelCache* kernel, double sigmaPx, unsigned width, unsigned height,
                                int upscale)
{
   spectra_.reset();
   if (width == 0 || height == 0)
      return false;
   W_ = width;
   H_ = height;
   const bool useKernel = kernel && kernel->valid;
   std::unique_ptr<WidefieldPsf> psf;
   if (useKernel)
   {
      u_ = KernelWidefieldPsf::ValidUpscale(kernel->oversampling, upscale);
      psf.reset(new KernelWidefieldPsf(*kernel, u_));
      R_ = psf->Radius(0, psf->MaxPlane());
      // The halo cut bounds what the splat draws; the grid kernel stops there too
      // (the cells inside keep the whole kernel's values).
      if (kernel->halo && !kernel->halo->radius2.empty())
      {
         const int32_t r2 = *std::max_element(kernel->halo->radius2.begin(), kernel->halo->radius2.end());
         if (r2 >= 0)
            R_ = std::min(R_, static_cast<int>(std::ceil((std::sqrt(static_cast<double>(r2)) + 1.0) * u_)));
      }
   }
   else
   {
      if (!(sigmaPx > 0.0))
         return false;
      u_ = std::max(1, std::min(upscale, 8));
      R_ = std::max(1, static_cast<int>(std::ceil(3.0 * sigmaPx * u_)));
   }
   R_ = std::max(1, R_);
   SpectraKey key;
   key.planes = useKernel ? kernel->Serial() : 0;
   key.splat = useKernel ? kernel->SplatSerial() : 0;
   key.u = u_;
   key.R = R_;
   key.W = W_;
   key.H = H_;
   key.sigma = useKernel ? 0.0 : sigmaPx;
   const int u = u_, R = R_;
   spectra_ = AcquireSpectra(key, [&]() {
      auto s = std::make_shared<BinnedKernelSpectra>();
      s->u = u;
      s->R = R;
      // Deposits span the FOV's cells plus R on each side; a grid that wide
      // keeps the circular convolution from wrapping into the cropped FOV.
      s->nx = RealFft2d::FastSize(W_ * static_cast<unsigned>(u) + 2u * static_cast<unsigned>(R), 2, true);
      s->ny = RealFft2d::FastSize(H_ * static_cast<unsigned>(u) + 2u * static_cast<unsigned>(R), 2, true);
      s->fft = RealFft2d(s->nx, s->ny);
      if (psf)
      {
         s->nz = psf->MaxPlane() + 1;
         s->psf = std::move(psf);
      }
      else
      {
         s->nz = 1;
         const int D = 2 * R + 1;
         const double sc = sigmaPx * u, twoS2 = 2.0 * sc * sc;
         std::vector<double> k(static_cast<size_t>(D) * D);
         double sum = 0.0;
         for (int y = -R; y <= R; ++y)
            for (int x = -R; x <= R; ++x)
            {
               const double v = std::exp(-(static_cast<double>(x) * x + static_cast<double>(y) * y) / twoS2);
               k[static_cast<size_t>(y + R) * D + (x + R)] = v;
               sum += v;
            }
         s->gauss.resize(k.size());
         for (size_t i = 0; i < k.size(); ++i)
            s->gauss[i] = static_cast<float>(k[i] / sum);
      }
      s->once.reset(new std::once_flag[static_cast<size_t>(s->nz)]);
      s->spec.resize(static_cast<size_t>(s->nz));
      return s;
   });
   return true;
}

void BinnedBlinkRenderer::Render(const std::vector<FrameEmitter>& emitters, std::vector<float>& img) const
{
   if (!spectra_ || emitters.empty())
      return;
   BinnedKernelSpectra& S = *spectra_;
   const int u = u_, R = R_;
   const int gw = static_cast<int>(W_) * u + 2 * R, gh = static_cast<int>(H_) * u + 2 * R;
   // Emitters per plane, in event order.
   std::vector<std::vector<uint32_t>> byPlane(static_cast<size_t>(S.nz));
   for (size_t i = 0; i < emitters.size(); ++i)
   {
      const FrameEmitter& e = emitters[i];
      if (!(e.photons > 0.0))
         continue;
      const int p = std::max(0, std::min(S.nz - 1, e.zIndex));
      byPlane[static_cast<size_t>(p)].push_back(static_cast<uint32_t>(i));
   }
   std::vector<float> dens(static_cast<size_t>(gw) * gh, 0.0f);
   std::vector<cfloat> acc(S.fft.SpecSize(), cfloat(0.0f, 0.0f)), tmp(S.fft.SpecSize());
   std::vector<size_t> touched;
   bool any = false;
   for (int p = 0; p < S.nz; ++p)
   {
      const std::vector<uint32_t>& list = byPlane[static_cast<size_t>(p)];
      if (list.empty())
         continue;
      touched.clear();
      for (uint32_t i : list)
      {
         const FrameEmitter& e = emitters[i];
         // Pixel x spans [x - 0.5, x + 0.5): cell floor((x + 0.5) u), whose
         // centre is where KernelWidefieldPsf centres the kernel.
         const int cx = static_cast<int>(std::floor((e.xPx + 0.5) * u)) + R;
         const int cy = static_cast<int>(std::floor((e.yPx + 0.5) * u)) + R;
         if (cx < 0 || cy < 0 || cx >= gw || cy >= gh)
            continue;
         const size_t c = static_cast<size_t>(cy) * gw + cx;
         if (dens[c] == 0.0f)
            touched.push_back(c);
         dens[c] += static_cast<float>(e.photons);
      }
      if (touched.empty())
         continue;
      any = true;
      S.fft.Forward(dens.data(), static_cast<unsigned>(gw), static_cast<unsigned>(gh), static_cast<size_t>(gw),
                    tmp.data(), true);
      const std::vector<cfloat>& K = S.Spectrum(p);
      for (size_t i = 0; i < acc.size(); ++i)
         acc[i] += tmp[i] * K[i];
      for (size_t c : touched)
         dens[c] = 0.0f;
   }
   if (!any)
      return;
   const unsigned cw = W_ * static_cast<unsigned>(u), ch = H_ * static_cast<unsigned>(u);
   std::vector<float> cells(static_cast<size_t>(cw) * ch);
   S.fft.Inverse(acc.data(), cells.data(), static_cast<unsigned>(R), ch, static_cast<unsigned>(R), cw, cw);
   for (unsigned y = 0; y < H_; ++y)
      for (unsigned x = 0; x < W_; ++x)
      {
         double sum = 0.0;
         for (int dy = 0; dy < u; ++dy)
         {
            const float* row = cells.data() + static_cast<size_t>(y * u + dy) * cw + static_cast<size_t>(x) * u;
            for (int dx = 0; dx < u; ++dx)
               sum += std::max(0.0f, row[dx]);
         }
         img[static_cast<size_t>(y) * W_ + x] += static_cast<float>(sum);
      }
}

} // namespace sim
