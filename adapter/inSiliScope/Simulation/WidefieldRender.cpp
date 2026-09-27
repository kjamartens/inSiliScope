///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldRender.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See WidefieldRender.h.
//
// LICENSE:       BSD (see license.txt)

#include "WidefieldRender.h"

#include "CellFieldSource.h"
#include "PsfGeneratorBridge.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace sim {

namespace {

constexpr double kAvogadro = 6.02214076e23;
// Spectra are summed over at most this many fixed chunks of PSF planes, then
// added in chunk order: the result does not depend on the thread count.
constexpr unsigned kMaxChunks = 8;
// Largest dye grid (floats per population) BuildWidefieldDyeGrid allocates.
constexpr double kMaxGridFloats = 400e6;

template <class Fn>
void ParallelFor(unsigned n, Fn fn)
{
#if defined(__EMSCRIPTEN__)
   for (unsigned i = 0; i < n; ++i)
      fn(i);
#else
   const unsigned T = std::max(1u, std::min(n, std::thread::hardware_concurrency()));
   std::atomic<unsigned> next{0};
   auto worker = [&]() {
      for (unsigned i; (i = next.fetch_add(1)) < n;)
         fn(i);
   };
   std::vector<std::thread> pool;
   for (unsigned t = 1; t < T; ++t)
      pool.emplace_back(worker);
   worker();
   for (std::thread& t : pool)
      t.join();
#endif
}

} // namespace

// ---- Photophysics ----------------------------------------------------------

double WidefieldPhotophysics::CrossSectionUm2() const
{
   // ln(10) * 1000 * eps / N_A in cm^2, x 1e8 um^2/cm^2.
   return std::log(10.0) * 1000.0 * extinctionCoeff / kAvogadro * 1e8;
}

double WidefieldPhotophysics::EmissionRatePerSec(double I) const
{
   return quantumYield * CrossSectionUm2() * excitationPhotonsPerUm2PerSec * I;
}

double WidefieldPhotophysics::HalfTimeSec(double I) const
{
   const double k = EmissionRatePerSec(I);
   if (!Bleaches() || !(k > 0.0))
      return std::numeric_limits<double>::infinity();
   return photonBudget * std::log(2.0) / k;
}

double WidefieldCollectionEfficiency(double na, double immersionIndex)
{
   const double r = std::min(1.0, std::max(0.0, na / std::max(1e-6, immersionIndex)));
   return 0.5 * (1.0 - std::sqrt(1.0 - r * r));
}

double WidefieldBleachingPhotons(double eta, double budget, double d0, double dD)
{
   if (!(budget > 0.0))
      return eta * dD;
   return eta * budget * std::exp(-d0 / budget) * (1.0 - std::exp(-dD / budget));
}

double WidefieldGaussianSigmaUm(double defocusUm, double lambdaNm, double na, double immersionIndex)
{
   // TODO(human): the emission PSF's width at a defocus. This placeholder is
   // the in-focus sigma (the cli's 0.21 lambda / NA) and ignores z, so the
   // Gaussian WideField image has no out-of-focus haze yet. Candidates:
   //   Gaussian-beam law     sigma0 * sqrt(1 + (dz / zR)^2)
   //   geometric defocus cone sqrt(sigma0^2 + (dz NA / (2 sqrt(n^2 - NA^2)))^2)
   //   or a blend of the two.
   (void)defocusUm;
   (void)immersionIndex;
   return 0.21 * lambdaNm / std::max(0.01, na) / 1000.0;
}

// ---- Dye grid --------------------------------------------------------------

bool BuildWidefieldDyeGrid(CellFieldSource& src, const WidefieldGridSpec& spec, WidefieldDyeGrid& out,
                           std::string& err)
{
   out = WidefieldDyeGrid();
   out.spec = spec;
   const double dz = spec.zPlaneUm;
   if (!(dz > 0.0) || spec.nx == 0 || spec.ny == 0 || !(spec.zMaxUm > spec.zMinUm))
   {
      err = "WideField: bad dye grid";
      return false;
   }
   const long kLo = static_cast<long>(std::floor(spec.zMinUm / dz));
   const long kHi = static_cast<long>(std::ceil(spec.zMaxUm / dz));
   const long nH = kHi - kLo;
   if (nH <= 0 || nH > 4000000)
   {
      err = "WideField: too many z planes (raise the plane thickness or narrow the z range)";
      return false;
   }
   const double x1 = spec.x0Um + spec.nx * spec.pitchUm, y1 = spec.y0Um + spec.ny * spec.pitchUm;
   std::vector<float> hist(static_cast<size_t>(nH));
   const long n = src.Density3d(spec.x0Um, spec.y0Um, x1, y1, kLo * dz, kHi * dz, 1, 1, static_cast<int>(nH),
                                ISC_POP_BLEACHING | ISC_POP_PERSISTENT, hist.data());
   if (n < 0)
   {
      err = "WideField: dye density query failed";
      return false;
   }
   if (n == 0)
      return true;
   long first = 0, last = nH - 1;
   while (first < nH && hist[static_cast<size_t>(first)] == 0.0f)
      ++first;
   while (last > first && hist[static_cast<size_t>(last)] == 0.0f)
      --last;
   // One spare plane each side: a dye on a plane boundary may bin either way.
   first = std::max(0L, first - 1);
   last = std::min(nH - 1, last + 1);
   out.k0 = kLo + first;
   out.nz = static_cast<unsigned>(last - first + 1);
   const double cells = static_cast<double>(spec.nx) * spec.ny * out.nz;
   if (cells > kMaxGridFloats)
   {
      err = "WideField: dye grid too large (lower the upscaling, raise the plane thickness or set a z range)";
      return false;
   }
   const double zA = out.k0 * dz, zB = (out.k0 + static_cast<long>(out.nz)) * dz;
   for (int pop : {ISC_POP_BLEACHING, ISC_POP_PERSISTENT})
   {
      std::vector<float>& g = pop == ISC_POP_BLEACHING ? out.bleaching : out.persistent;
      g.assign(static_cast<size_t>(cells), 0.0f);
      const long c = src.Density3d(spec.x0Um, spec.y0Um, x1, y1, zA, zB, static_cast<int>(spec.nx),
                                   static_cast<int>(spec.ny), static_cast<int>(out.nz), pop, g.data());
      if (c < 0)
      {
         err = "WideField: dye density query failed";
         return false;
      }
      if (c == 0)
         std::vector<float>().swap(g);
      (pop == ISC_POP_BLEACHING ? out.nBleaching : out.nPersistent) = c;
   }
   return true;
}

// ---- PSF providers -----------------------------------------------------------

GaussianWidefieldPsf::GaussianWidefieldPsf(double pitchUm, double lambdaNm, double na, double immersionIndex,
                                           double stepUm)
   : pitch_(pitchUm), lambdaNm_(lambdaNm), na_(na), n_(immersionIndex), step_(stepUm > 0.0 ? stepUm : 0.1)
{
}

double GaussianWidefieldPsf::Sigma(int p) const
{
   return WidefieldGaussianSigmaUm(p * step_, lambdaNm_, na_, n_);
}

int GaussianWidefieldPsf::Radius(int pLo, int pHi) const
{
   double s = std::max(Sigma(pLo), Sigma(pHi));
   if (pLo <= 0 && pHi >= 0)
      s = std::max(s, Sigma(0));
   return std::max(1, static_cast<int>(std::ceil(3.0 * s / pitch_)));
}

void GaussianWidefieldPsf::Kernel(int p, int R, std::vector<float>& out) const
{
   const int D = 2 * R + 1;
   out.assign(static_cast<size_t>(D) * D, 0.0f);
   const double s = Sigma(p) / pitch_, twoS2 = 2.0 * s * s;
   std::vector<double> k(out.size());
   double sum = 0.0;
   for (int y = -R; y <= R; ++y)
      for (int x = -R; x <= R; ++x)
      {
         const double v = std::exp(-(static_cast<double>(x) * x + static_cast<double>(y) * y) / twoS2);
         k[static_cast<size_t>(y + R) * D + (x + R)] = v;
         sum += v;
      }
   for (size_t i = 0; i < k.size(); ++i)
      out[i] = static_cast<float>(k[i] / sum);
}

int VectorialWidefieldPsf::ValidUpscale(int oversampling, int requested)
{
   const int os = std::max(1, oversampling);
   for (int u = std::max(1, std::min(requested, os)); u > 1; --u)
      if (os % u == 0)
         return u;
   return 1;
}

VectorialWidefieldPsf::VectorialWidefieldPsf(const PsfKernelCache& cache, int upscale) : c_(cache)
{
   const int os = std::max(1, cache.oversampling);
   r_ = std::max(1, os / ValidUpscale(os, upscale));
   // As SplatSetup (Nearest) for a dye at a cell centre: tx = kc - r/2 + 0.5,
   // rounded half up.
   const double kc = (cache.sizeOversampled - 1) / 2.0;
   s0_ = static_cast<int>(std::floor(kc - r_ / 2.0 + 1.0));
}

double VectorialWidefieldPsf::PlaneCoord(double defocusUm) const
{
   if (c_.nz <= 1 || !(c_.zStepNm > 0.0))
      return 0.0;
   return defocusUm * 1000.0 / c_.zStepNm + (c_.nz - 1) / 2.0;
}

int VectorialWidefieldPsf::MaxPlane() const
{
   return std::max(0, c_.nz - 1);
}

int VectorialWidefieldPsf::Radius(int, int) const
{
   const int n = c_.sizeOversampled;
   // Grid cells d whose block [s0 + d r, s0 + d r + r - 1] overlaps [0, n).
   const int dMin = static_cast<int>(std::ceil((-s0_ - r_ + 1) / static_cast<double>(r_)));
   const int dMax = static_cast<int>(std::floor((n - 1 - s0_) / static_cast<double>(r_)));
   return std::max(1, std::max(-dMin, dMax));
}

void VectorialWidefieldPsf::Kernel(int p, int R, std::vector<float>& out) const
{
   const int D = 2 * R + 1, n = c_.sizeOversampled;
   out.assign(static_cast<size_t>(D) * D, 0.0f);
   if (p < 0 || p >= static_cast<int>(c_.planes.size()))
      return;
   const std::vector<float>& P = c_.planes[static_cast<size_t>(p)];
   for (int dy = -R; dy <= R; ++dy)
   {
      const int ya = s0_ + dy * r_, yb = std::min(n, ya + r_);
      for (int dx = -R; dx <= R; ++dx)
      {
         const int xa = s0_ + dx * r_, xb = std::min(n, xa + r_);
         double acc = 0.0;
         for (int y = std::max(0, ya); y < yb; ++y)
            for (int x = std::max(0, xa); x < xb; ++x)
               acc += P[static_cast<size_t>(y) * n + x];
         out[static_cast<size_t>(dy + R) * D + (dx + R)] = static_cast<float>(acc);
      }
   }
}

// ---- Bleach field ------------------------------------------------------------

void BleachField::Reset(double pitchUm)
{
   tiles_.clear();
   pitch_ = pitchUm;
}

void BleachField::DoseOver(double x0Um, double y0Um, unsigned nx, unsigned ny, std::vector<float>& out) const
{
   out.assign(static_cast<size_t>(nx) * ny, 0.0f);
   if (tiles_.empty() || !(pitch_ > 0.0))
      return;
   const long long cx0 = static_cast<long long>(std::floor(x0Um / pitch_ + 0.5));
   const long long cy0 = static_cast<long long>(std::floor(y0Um / pitch_ + 0.5));
   for (unsigned iy = 0; iy < ny; ++iy)
   {
      const long long cy = cy0 + iy;
      const long long ty = cy >= 0 ? cy / kTile : -((-cy + kTile - 1) / kTile);
      for (unsigned ix = 0; ix < nx; ++ix)
      {
         const long long cx = cx0 + ix;
         const long long tx = cx >= 0 ? cx / kTile : -((-cx + kTile - 1) / kTile);
         auto it = tiles_.find({tx, ty});
         if (it == tiles_.end())
            continue;
         out[static_cast<size_t>(iy) * nx + ix] =
            it->second[static_cast<size_t>(cy - ty * kTile) * kTile + static_cast<size_t>(cx - tx * kTile)];
      }
   }
}

void BleachField::Deposit(const IlluminationPattern& pattern, double axisXUm, double axisYUm, double dDPerUnitI)
{
   if (!(pitch_ > 0.0) || !(dDPerUnitI > 0.0))
      return;
   double sx0, sy0, sx1, sy1;
   pattern.Support(sx0, sy0, sx1, sy1);
   const long long cx0 = static_cast<long long>(std::floor((axisXUm + sx0) / pitch_));
   const long long cx1 = static_cast<long long>(std::floor((axisXUm + sx1) / pitch_));
   const long long cy0 = static_cast<long long>(std::floor((axisYUm + sy0) / pitch_));
   const long long cy1 = static_cast<long long>(std::floor((axisYUm + sy1) / pitch_));
   for (long long cy = cy0; cy <= cy1; ++cy)
   {
      const long long ty = cy >= 0 ? cy / kTile : -((-cy + kTile - 1) / kTile);
      const double dy = (cy + 0.5) * pitch_ - axisYUm;
      for (long long cx = cx0; cx <= cx1; ++cx)
      {
         const double I = pattern.At((cx + 0.5) * pitch_ - axisXUm, dy);
         if (!(I > 0.0))
            continue;
         const long long tx = cx >= 0 ? cx / kTile : -((-cx + kTile - 1) / kTile);
         std::vector<float>& t = tiles_[{tx, ty}];
         if (t.empty())
            t.assign(static_cast<size_t>(kTile) * kTile, 0.0f);
         t[static_cast<size_t>(cy - ty * kTile) * kTile + static_cast<size_t>(cx - tx * kTile)] +=
            static_cast<float>(dDPerUnitI * I);
      }
   }
}

// ---- Scene -------------------------------------------------------------------

WidefieldGridSpec WidefieldScene::GridSpecFor(const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                                             unsigned* marginX, unsigned* marginY)
{
   const int u = std::max(1, spec.grid.upscale);
   const double pitch = spec.pixelUm / u;
   // Pixel X's centre is at originX + X pixelUm (the SR renderer's
   // convention), so the FOV spans [originX - pixel/2, ...); the axis is its
   // centre.
   const double axisX = spec.originXUm + (spec.width - 1) / 2.0 * spec.pixelUm;
   const double axisY = spec.originYUm + (spec.height - 1) / 2.0 * spec.pixelUm;
   const double fx0 = spec.originXUm - spec.pixelUm / 2, fy0 = spec.originYUm - spec.pixelUm / 2;
   const double fx1 = fx0 + spec.width * spec.pixelUm, fy1 = fy0 + spec.height * spec.pixelUm;
   // Margin beyond the FOV where the pattern still excites (dyes elsewhere
   // are dark), at most marginUm.
   double sx0, sy0, sx1, sy1;
   pattern.Support(sx0, sy0, sx1, sy1);
   const long mCap = static_cast<long>(std::ceil(std::max(0.0, spec.marginUm) / pitch - 1e-9));
   auto marg = [&](double excess) {
      return static_cast<unsigned>(std::min(mCap, std::max(0L, static_cast<long>(std::ceil(excess / pitch - 1e-9)))));
   };
   const unsigned mx = std::max(marg(fx0 - (axisX + sx0)), marg(axisX + sx1 - fx1));
   const unsigned my = std::max(marg(fy0 - (axisY + sy0)), marg(axisY + sy1 - fy1));
   if (marginX)
      *marginX = mx;
   if (marginY)
      *marginY = my;

   WidefieldGridSpec g;
   g.pitchUm = pitch;
   g.x0Um = fx0 - mx * pitch;
   g.y0Um = fy0 - my * pitch;
   g.nx = spec.width * u + 2 * mx;
   g.ny = spec.height * u + 2 * my;
   g.zPlaneUm = std::max(1e-3, spec.grid.zPlaneNm / 1000.0);
   if (spec.slabHalfUm > 0.0)
   {
      g.zMinUm = spec.slabCentreUm - spec.slabHalfUm;
      g.zMaxUm = spec.slabCentreUm + spec.slabHalfUm;
   }
   else
   {
      g.zMinUm = -5.0;
      g.zMaxUm = 50.0;
   }
   return g;
}

bool WidefieldScene::Update(CellFieldSource& src, const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                            const WidefieldPsf& psf, std::string& err)
{
   if (spec.width == 0 || spec.height == 0 || !(spec.pixelUm > 0.0))
   {
      err = "WideField: bad frame";
      return false;
   }
   unsigned mx = 0, my = 0;
   const WidefieldGridSpec g = GridSpecFor(pattern, spec, &mx, &my);
   const bool gridChanged = !haveSpec_ || g != grid_.spec || spec.worldVersion != spec_.worldVersion;
   if (gridChanged)
   {
      haveSpec_ = false; // a failure below leaves no half-built state
      if (!BuildWidefieldDyeGrid(src, g, grid_, err))
         return false;
   }
   Finish(gridChanged, pattern, spec, psf, mx, my);
   return true;
}

bool WidefieldScene::UpdateFromGrid(const WidefieldDyeGrid& grid, const IlluminationPattern& pattern,
                                    const WidefieldSceneSpec& spec, const WidefieldPsf& psf, std::string& err)
{
   unsigned mx = 0, my = 0;
   if (spec.width == 0 || spec.height == 0 || !(spec.pixelUm > 0.0) || GridSpecFor(pattern, spec, &mx, &my) != grid.spec)
   {
      err = "WideField: dye grid does not match the scene";
      return false;
   }
   grid_ = grid;
   Finish(true, pattern, spec, psf, mx, my);
   return true;
}

void WidefieldScene::Finish(bool gridChanged, const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                            const WidefieldPsf& psf, unsigned mx, unsigned my)
{
   const WidefieldGridSpec& g = grid_.spec;
   const double pitch = g.pitchUm;
   axisX_ = spec.originXUm + (spec.width - 1) / 2.0 * spec.pixelUm;
   axisY_ = spec.originYUm + (spec.height - 1) / 2.0 * spec.pixelUm;
   fovX0_ = mx;
   fovY0_ = my;

   // Illumination and the frame dose per grid column.
   const size_t n2 = static_cast<size_t>(g.nx) * g.ny;
   std::vector<float> ill(n2), dD(n2), wp(n2);
   pattern.Sample(g.x0Um - axisX_, g.y0Um - axisY_, pitch, g.nx, g.ny, ill.data());
   const double dD1 = spec.phot.EmissionRatePerSec(1.0) * spec.exposureSec;
   for (size_t i = 0; i < n2; ++i)
   {
      dD[i] = static_cast<float>(dD1 * ill[i]);
      wp[i] = static_cast<float>(spec.eta * dD[i]);
   }

   const bool planesChanged = gridChanged || spec.focusWorldUm != spec_.focusWorldUm ||
                              spec.psfVersion != spec_.psfVersion || spec.kernelCapUm != spec_.kernelCapUm;
   spec_ = spec;
   illum_.swap(ill);
   dD_.swap(dD);
   if (planesChanged)
   {
      BuildPlanes(psf, spec.kernelCapUm);
      bleachValid_ = false;
   }
   if (planesChanged || wp != wp_)
   {
      wp_.swap(wp);
      Spectrum(srcP_, wp_, Sp_);
   }
   haveSpec_ = true;
}

void WidefieldScene::BuildPlanes(const WidefieldPsf& psf, double kernelCapUm)
{
   const WidefieldGridSpec& g = grid_.spec;
   const size_t n2 = static_cast<size_t>(g.nx) * g.ny;
   const bool hasB = !grid_.bleaching.empty(), hasP = !grid_.persistent.empty();
   // Each occupied dye plane goes to PSF planes p0 and p0 + 1 with linear weights.
   struct Dep { unsigned i; int p0; double frac; };
   std::vector<Dep> deps;
   clamped_ = 0;
   int pLo = std::numeric_limits<int>::max(), pHi = std::numeric_limits<int>::min();
   for (unsigned i = 0; i < grid_.nz; ++i)
   {
      double count = 0.0;
      for (size_t c = 0; c < n2; ++c)
         count += (hasB ? grid_.bleaching[i * n2 + c] : 0.0f) + (hasP ? grid_.persistent[i * n2 + c] : 0.0f);
      if (count == 0.0)
         continue;
      double t = psf.PlaneCoord(grid_.PlaneCentreUm(i) - spec_.focusWorldUm);
      if (t < psf.MinPlane() || t > psf.MaxPlane())
      {
         clamped_ += static_cast<long>(count);
         t = std::min<double>(psf.MaxPlane(), std::max<double>(psf.MinPlane(), t));
      }
      int p0 = static_cast<int>(std::floor(t));
      double frac = t - p0;
      if (p0 >= psf.MaxPlane())
      {
         p0 = psf.MaxPlane();
         frac = 0.0;
      }
      deps.push_back({i, p0, frac});
      pLo = std::min(pLo, p0);
      pHi = std::max(pHi, frac > 0.0 ? p0 + 1 : p0);
   }
   nP_ = deps.empty() ? 0 : pHi - pLo + 1;
   pMin_ = deps.empty() ? 0 : pLo;
   srcB_.assign(hasB ? static_cast<size_t>(nP_) * n2 : 0, 0.0f);
   srcP_.assign(hasP ? static_cast<size_t>(nP_) * n2 : 0, 0.0f);
   for (const Dep& d : deps)
      for (int s = 0; s < 2; ++s)
      {
         const double w = s == 0 ? 1.0 - d.frac : d.frac;
         if (w == 0.0)
            continue;
         const size_t dst = static_cast<size_t>(d.p0 + s - pMin_) * n2, from = static_cast<size_t>(d.i) * n2;
         for (size_t c = 0; c < n2; ++c)
         {
            if (hasB)
               srcB_[dst + c] += static_cast<float>(w * grid_.bleaching[from + c]);
            if (hasP)
               srcP_[dst + c] += static_cast<float>(w * grid_.persistent[from + c]);
         }
      }

   const int cap = std::max(1, static_cast<int>(std::ceil(kernelCapUm / g.pitchUm - 1e-9)));
   R_ = nP_ > 0 ? std::max(1, std::min(cap, psf.Radius(pLo, pHi))) : 1;
   const int D = 2 * R_ + 1;
   kernels_.assign(static_cast<size_t>(nP_) * D * D, 0.0f);
   std::vector<float> k;
   for (int p = 0; p < nP_; ++p)
   {
      psf.Kernel(pMin_ + p, R_, k);
      std::copy(k.begin(), k.end(), kernels_.begin() + static_cast<size_t>(p) * D * D);
   }
   // Linear (not circular) convolution over the FOV cells.
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   const unsigned need = std::max({g.nx, g.ny, spec_.width * u + fovX0_ + R_ + 1, spec_.height * u + fovY0_ + R_ + 1});
   if (Fft2d::NextPow2(need) != fft_.N())
      fft_ = Fft2d(need);
}

void WidefieldScene::Spectrum(const std::vector<float>& src, const std::vector<float>& w,
                              std::vector<cfloat>& S) const
{
   const size_t NN = fft_.Size();
   const unsigned N = fft_.N();
   const WidefieldGridSpec& g = grid_.spec;
   const size_t n2 = static_cast<size_t>(g.nx) * g.ny;
   // Empty = nothing to add (RenderScaled skips it).
   S.clear();
   if (src.empty() || nP_ == 0)
      return;
   std::vector<int> active;
   for (int p = 0; p < nP_; ++p)
   {
      double s = 0.0;
      for (size_t c = 0; c < n2; ++c)
         s += static_cast<double>(src[static_cast<size_t>(p) * n2 + c]) * w[c];
      if (s != 0.0)
         active.push_back(p);
   }
   if (active.empty())
      return;
   S.assign(NN, cfloat(0.0f, 0.0f));
   const unsigned nPairs = static_cast<unsigned>((active.size() + 1) / 2);
   const unsigned nChunks = std::min(kMaxChunks, nPairs);
   std::vector<std::vector<cfloat>> partial(nChunks);
   const int R = R_, D = 2 * R + 1;
   ParallelFor(nChunks, [&](unsigned ch) {
      std::vector<cfloat> zs(NN), zk(NN);
      std::vector<cfloat>& acc = partial[ch];
      acc.assign(NN, cfloat(0.0f, 0.0f));
      const unsigned q0 = ch * nPairs / nChunks, q1 = (ch + 1) * nPairs / nChunks;
      for (unsigned q = q0; q < q1; ++q)
      {
         const int pa = active[2 * q];
         const int pb = 2 * q + 1 < active.size() ? active[2 * q + 1] : -1;
         std::fill(zs.begin(), zs.end(), cfloat(0.0f, 0.0f));
         std::fill(zk.begin(), zk.end(), cfloat(0.0f, 0.0f));
         const float* sa = &src[static_cast<size_t>(pa) * n2];
         const float* sb = pb >= 0 ? &src[static_cast<size_t>(pb) * n2] : nullptr;
         for (unsigned iy = 0; iy < g.ny; ++iy)
            for (unsigned ix = 0; ix < g.nx; ++ix)
            {
               const size_t c = static_cast<size_t>(iy) * g.nx + ix;
               zs[static_cast<size_t>(iy) * N + ix] = cfloat(sa[c] * w[c], sb ? sb[c] * w[c] : 0.0f);
            }
         const float* ka = &kernels_[static_cast<size_t>(pa) * D * D];
         const float* kb = pb >= 0 ? &kernels_[static_cast<size_t>(pb) * D * D] : nullptr;
         for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx)
            {
               const size_t kIdx = static_cast<size_t>(dy + R) * D + (dx + R);
               const size_t at = static_cast<size_t>((dy + static_cast<int>(N)) % static_cast<int>(N)) * N +
                                 static_cast<size_t>((dx + static_cast<int>(N)) % static_cast<int>(N));
               zk[at] = cfloat(ka[kIdx], kb ? kb[kIdx] : 0.0f);
            }
         fft_.Forward(zs.data());
         fft_.Forward(zk.data());
         // Split both pairs by Hermitian symmetry; acc += A1 P1 + A2 P2.
         for (size_t k = 0; k < NN; ++k)
         {
            const size_t m = fft_.Neg(k);
            const float sr = zs[k].real(), si = zs[k].imag(), tr = zs[m].real(), ti = zs[m].imag();
            const float kr = zk[k].real(), ki = zk[k].imag(), lr = zk[m].real(), li = zk[m].imag();
            const float a1r = 0.5f * (sr + tr), a1i = 0.5f * (si - ti);
            const float a2r = 0.5f * (si + ti), a2i = -0.5f * (sr - tr);
            const float p1r = 0.5f * (kr + lr), p1i = 0.5f * (ki - li);
            const float p2r = 0.5f * (ki + li), p2i = -0.5f * (kr - lr);
            const float re = (a1r * p1r - a1i * p1i) + (a2r * p2r - a2i * p2i);
            const float im = (a1r * p1i + a1i * p1r) + (a2r * p2i + a2i * p2r);
            acc[k] = cfloat(acc[k].real() + re, acc[k].imag() + im);
         }
      }
   });
   for (unsigned ch = 0; ch < nChunks; ++ch)
      for (size_t k = 0; k < NN; ++k)
         S[k] = cfloat(S[k].real() + partial[ch][k].real(), S[k].imag() + partial[ch][k].imag());
}

void WidefieldScene::FreshBleachWeights(double framesBefore, std::vector<float>& wb) const
{
   wb.resize(dD_.size());
   const double B = spec_.phot.photonBudget;
   for (size_t i = 0; i < dD_.size(); ++i)
      wb[i] = static_cast<float>(WidefieldBleachingPhotons(spec_.eta, B, framesBefore * dD_[i], dD_[i]));
}

void WidefieldScene::BleachWeightsFromDose(const std::vector<float>& d0, std::vector<float>& wb) const
{
   wb.resize(dD_.size());
   const double B = spec_.phot.photonBudget;
   for (size_t i = 0; i < dD_.size(); ++i)
      wb[i] = static_cast<float>(WidefieldBleachingPhotons(spec_.eta, B, i < d0.size() ? d0[i] : 0.0f, dD_[i]));
}

void WidefieldScene::SetBleachWeights(const std::vector<float>& wb)
{
   wbRef_ = wb;
   Spectrum(srcB_, wbRef_, Sb_);
   bleachValid_ = true;
}

bool WidefieldScene::ScalarOfBleaching(const std::vector<float>& wb, double& c) const
{
   if (!bleachValid_ || wb.size() != wbRef_.size())
      return false;
   size_t iMax = 0;
   float refMax = 0.0f, wMax = 0.0f;
   for (size_t i = 0; i < wb.size(); ++i)
   {
      if (std::fabs(wbRef_[i]) > refMax)
      {
         refMax = std::fabs(wbRef_[i]);
         iMax = i;
      }
      wMax = std::max(wMax, std::fabs(wb[i]));
   }
   if (refMax == 0.0f)
   {
      c = 1.0;
      return wMax == 0.0f;
   }
   c = static_cast<double>(wb[iMax]) / wbRef_[iMax];
   const double tol = 1e-5 * std::max(static_cast<double>(wMax), 1e-30);
   for (size_t i = 0; i < wb.size(); ++i)
      if (std::fabs(wb[i] - c * wbRef_[i]) > tol)
         return false;
   return true;
}

void WidefieldScene::RenderScaled(double c, std::vector<float>& cam, std::vector<cfloat>& scratch) const
{
   const bool useB = bleachValid_ && !Sb_.empty() && c != 0.0, useP = !Sp_.empty();
   if (!useB && !useP)
      return;
   const size_t NN = fft_.Size();
   const unsigned N = fft_.N();
   scratch.resize(NN);
   const float cf = static_cast<float>(c);
   for (size_t k = 0; k < NN; ++k)
   {
      float re = 0.0f, im = 0.0f;
      if (useB)
      {
         re = cf * Sb_[k].real();
         im = cf * Sb_[k].imag();
      }
      if (useP)
      {
         re += Sp_[k].real();
         im += Sp_[k].imag();
      }
      scratch[k] = cfloat(re, im);
   }
   fft_.Inverse(scratch.data());
   const unsigned u = static_cast<unsigned>(std::max(1, spec_.grid.upscale));
   const unsigned W = spec_.width, H = spec_.height;
   cam.resize(static_cast<size_t>(W) * H, 0.0f);
   for (unsigned Y = 0; Y < H; ++Y)
      for (unsigned X = 0; X < W; ++X)
      {
         double acc = 0.0;
         for (unsigned sy = 0; sy < u; ++sy)
         {
            const cfloat* row = &scratch[static_cast<size_t>(fovY0_ + Y * u + sy) * N + fovX0_ + X * u];
            for (unsigned sx = 0; sx < u; ++sx)
               acc += std::max(0.0f, row[sx].real());
         }
         cam[static_cast<size_t>(Y) * W + X] += static_cast<float>(acc);
      }
}

void WidefieldScene::RenderFrame(const std::vector<float>& wb, std::vector<float>& cam)
{
   double c = 0.0;
   lastFast_ = true;
   if (!srcB_.empty())
   {
      if (!ScalarOfBleaching(wb, c))
      {
         SetBleachWeights(wb);
         c = 1.0;
         lastFast_ = false;
      }
   }
   RenderScaled(c, cam, scratch_);
}

} // namespace sim
