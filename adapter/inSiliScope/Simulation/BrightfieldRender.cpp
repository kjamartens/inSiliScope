///////////////////////////////////////////////////////////////////////////////
// FILE:          BrightfieldRender.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See BrightfieldRender.h and spec/BRIGHTFIELD.md.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "BrightfieldRender.h"

#include "CellFieldSource.h"
#include "Parallel.h"
#include "ZernikePsf.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sim {

namespace {

const double kPi = 3.14159265358979323846;
// Slices held by one optical-volume query (bounds its 3-channel buffer).
constexpr double kVolumeQueryFloats = 48e6;
// Exit spectra kept per scene; above this they are recomputed per focus.
constexpr double kExitCacheBytes = 768e6;
// Slice transmittances kept per scene; above this they are made per source.
constexpr double kTransCacheBytes = 512e6;

double Ms(std::chrono::steady_clock::time_point t0)
{
   return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Signed frequency index of FFT bin i of n.
inline int Freq(unsigned i, unsigned n) { return i <= n / 2 ? static_cast<int>(i) : static_cast<int>(i) - static_cast<int>(n); }

} // namespace

BrightfieldQuality BrightfieldQualityLevel(int level)
{
   static const BrightfieldQuality kLevels[5] = {
      // Upscale 1: the grid is fine enough at pitch <= lambda / (4 n), which
      // UpscaleFor enforces (a finer grid did not change the image).
      {6, 1, 1, 0.0, 3.0},     // 1: thin object, few sources
      {12, 1, 1, 0.5, 3.0},    // 2
      {24, 1, 2, 0.5, 3.0},    // 3: default
      {48, 1, 2, 0.25, 4.0},   // 4
      {96, 1, 4, 0.125, 6.0},  // 5: reference level for checks; not exposed (cli/viewer/MM clamp to 4)
   };
   return kLevels[std::max(1, std::min(5, level)) - 1];
}

BrightfieldQuality BrightfieldSpec::Resolved() const
{
   BrightfieldQuality q = BrightfieldQualityLevel(quality);
   if (sources > 0)
      q.sources = sources;
   if (upscale > 0)
      q.upscale = upscale;
   if (sub > 0)
      q.sub = sub;
   if (sliceUm >= 0.0)
      q.sliceUm = sliceUm;
   if (marginUm > 0.0)
      q.marginUm = marginUm;
   if (condenserNa <= 0.0)
      q.sources = 1;
   return q;
}

static bool SameSpec(const BrightfieldSpec& a, const BrightfieldSpec& b)
{
   const BrightfieldQuality qa = a.Resolved(), qb = b.Resolved();
   return a.originXUm == b.originXUm && a.originYUm == b.originYUm && a.width == b.width && a.height == b.height &&
          a.pixelUm == b.pixelUm && qa.sources == qb.sources && qa.upscale == qb.upscale && qa.sub == qb.sub &&
          qa.sliceUm == qb.sliceUm && qa.marginUm == qb.marginUm && a.wavelengthNm == b.wavelengthNm &&
          a.na == b.na && a.condenserNa == b.condenserNa && a.zernike == b.zernike && a.nMedium == b.nMedium &&
          a.nCytoplasm == b.nCytoplasm && a.nNucleus == b.nNucleus && a.nMicrotubule == b.nMicrotubule &&
          a.absorptionPerUm == b.absorptionPerUm;
}

namespace {
// u[i] *= m[i] with std::complex's operations written out (ar*br - ai*bi,
// ar*bi + ai*br): the same bits for finite values, and a plain loop the
// compiler vectorises (libstdc++/libc++ route operator* through __mulsc3).
inline void MulInPlace(cfloat* u, const cfloat* m, size_t n)
{
   for (size_t i = 0; i < n; ++i)
   {
      const float ar = u[i].real(), ai = u[i].imag(), br = m[i].real(), bi = m[i].imag();
      u[i] = cfloat(ar * br - ai * bi, ar * bi + ai * br);
   }
}
} // namespace

// Rows [row0, row1): 1D FFTs along x, 16 rows at a time in a small
// interleaved buffer (cache resident).
void BrightfieldScene::FftRows(cfloat* a, std::vector<cfloat>& work, unsigned row0, unsigned row1, bool conjIn,
                               float outScale, bool conjOut) const
{
   constexpr unsigned B = 16;
   const unsigned nx = nx_;
   work.resize(2 * static_cast<size_t>(std::max(nx_, ny_)) * B);
   cfloat* buf = work.data();
   cfloat* tmp = buf + static_cast<size_t>(std::max(nx_, ny_)) * B;
   for (unsigned r0 = row0; r0 < row1; r0 += B)
   {
      const unsigned rb = std::min(B, row1 - r0);
      for (unsigned b = 0; b < B; ++b)
      {
         if (b >= rb)
         {
            for (unsigned x = 0; x < nx; ++x)
               buf[static_cast<size_t>(x) * B + b] = cfloat(0.0f, 0.0f);
            continue;
         }
         const cfloat* row = a + static_cast<size_t>(r0 + b) * nx;
         if (conjIn)
            for (unsigned x = 0; x < nx; ++x)
               buf[static_cast<size_t>(x) * B + b] = std::conj(row[x]);
         else
            for (unsigned x = 0; x < nx; ++x)
               buf[static_cast<size_t>(x) * B + b] = row[x];
      }
      const cfloat* r = planX_.Forward(buf, tmp, B);
      for (unsigned b = 0; b < rb; ++b)
      {
         cfloat* row = a + static_cast<size_t>(r0 + b) * nx;
         for (unsigned x = 0; x < nx; ++x)
         {
            const cfloat v = r[static_cast<size_t>(x) * B + b];
            row[x] = (conjOut ? std::conj(v) : v) * outScale;
         }
      }
   }
}

// The listed columns: 1D FFTs along y, 16 columns at a time.
void BrightfieldScene::FftCols(cfloat* a, std::vector<cfloat>& work, const std::vector<unsigned>& cols, bool conjIn,
                               bool conjOut) const
{
   constexpr unsigned B = 16;
   const unsigned nx = nx_, ny = ny_;
   work.resize(2 * static_cast<size_t>(std::max(nx_, ny_)) * B);
   cfloat* buf = work.data();
   cfloat* tmp = buf + static_cast<size_t>(std::max(nx_, ny_)) * B;
   const unsigned nc = static_cast<unsigned>(cols.size());
   for (unsigned c0 = 0; c0 < nc; c0 += B)
   {
      const unsigned cb = std::min(B, nc - c0);
      const unsigned* cl = &cols[c0];
      for (unsigned y = 0; y < ny; ++y)
      {
         const cfloat* row = a + static_cast<size_t>(y) * nx;
         cfloat* d = buf + static_cast<size_t>(y) * B;
         for (unsigned b = 0; b < cb; ++b)
            d[b] = conjIn ? std::conj(row[cl[b]]) : row[cl[b]];
         for (unsigned b = cb; b < B; ++b)
            d[b] = cfloat(0.0f, 0.0f);
      }
      const cfloat* r = planY_.Forward(buf, tmp, B);
      for (unsigned y = 0; y < ny; ++y)
      {
         cfloat* row = a + static_cast<size_t>(y) * nx;
         const cfloat* sv = r + static_cast<size_t>(y) * B;
         for (unsigned b = 0; b < cb; ++b)
            row[cl[b]] = conjOut ? std::conj(sv[b]) : sv[b];
      }
   }
}

void BrightfieldScene::FftForward(cfloat* a, std::vector<cfloat>& work, bool band) const
{
   FftRows(a, work, 0, ny_, false, 1.0f, false);
   FftCols(a, work, band ? bandCols_ : allCols_, false, false);
}

// Inverse = conj(F(conj(X))) / N; the input is zero outside the band columns.
void BrightfieldScene::FftInverse(cfloat* a, std::vector<cfloat>& work, unsigned row0, unsigned row1) const
{
   FftCols(a, work, bandCols_, true, false);
   FftRows(a, work, row0, row1, false, 1.0f / (static_cast<float>(nx_) * static_cast<float>(ny_)), true);
}

unsigned BrightfieldScene::UpscaleFor(const BrightfieldSpec& spec)
{
   const BrightfieldQuality q = spec.Resolved();
   const double maxPitch = spec.wavelengthNm * 1e-3 / (4.0 * std::max(1.0, spec.nMedium));
   const unsigned need = static_cast<unsigned>(std::ceil(spec.pixelUm / maxPitch - 1e-9));
   return std::min(8u, std::max({1u, static_cast<unsigned>(std::max(1, q.upscale)), need}));
}

void BrightfieldScene::GridFor(const BrightfieldSpec& spec, unsigned& nx, unsigned& ny, unsigned& marginCells)
{
   const BrightfieldQuality q = spec.Resolved();
   const unsigned up = UpscaleFor(spec);
   const double pitch = spec.pixelUm / up;
   marginCells = static_cast<unsigned>(std::ceil(q.marginUm / pitch));
   nx = RealFft2d::FastSize(spec.width * up + 2 * marginCells, 2);
   ny = RealFft2d::FastSize(spec.height * up + 2 * marginCells, 2);
}

bool BrightfieldScene::Begin(const BrightfieldSpec& spec, std::string& err)
{
   valid_ = false;
   haveImage_ = false;
   if (spec.width == 0 || spec.height == 0 || !(spec.pixelUm > 0) || !(spec.wavelengthNm > 0) || !(spec.na > 0) ||
       !(spec.nMedium > 0))
   {
      err = "BrightField: bad FOV, wavelength, NA or medium index.";
      return false;
   }
   spec_ = spec;
   up_ = UpscaleFor(spec);
   pitch_ = spec.pixelUm / up_;
   GridFor(spec, nx_, ny_, margin_);
   planX_ = FftPlan1d(nx_);
   planY_ = FftPlan1d(ny_);
   k0_ = 2.0 * kPi / (spec.wavelengthNm * 1e-3);
   return true;
}

bool BrightfieldScene::Update(CellFieldSource& src, const BrightfieldSpec& spec, uint64_t worldVersion,
                              std::string& err)
{
   if (valid_ && srcId_ == &src && worldVersion_ == worldVersion && SameSpec(spec_, spec))
      return true;
   const auto t0 = std::chrono::steady_clock::now();
   if (!Begin(spec, err))
      return false;
   const BrightfieldQuality q = spec.Resolved();
   const size_t N = static_cast<size_t>(nx_) * ny_;
   const double gx0 = spec.originXUm - margin_ * pitch_, gy0 = spec.originYUm - margin_ * pitch_;
   const double gx1 = gx0 + nx_ * pitch_, gy1 = gy0 + ny_ * pitch_;

   // Slices over [0, tallest cell].
   const double zTop = src.MaxCellHeight(gx0, gy0, gx1, gy1);
   if (zTop < 0)
   {
      err = "BrightField: the cell query failed.";
      return false;
   }
   slices_ = (q.sliceUm > 0.0 && zTop > 0.0) ? std::max(1, static_cast<int>(std::ceil(zTop / q.sliceUm))) : 1;
   dz_ = zTop > 0.0 ? zTop / slices_ : 1.0;
   objectZ_ = 0.5 * dz_;
   // One thin screen: queried as kThinSub sub-slices, summed, and placed at
   // the phase-weighted mean height (a flat lamella stays near the
   // coverslip, not at half the tallest cell).
   constexpr int kThinSub = 8;
   const int qSlices = slices_ == 1 && zTop > 0.0 ? kThinSub : slices_;
   const double qDz = zTop > 0.0 ? zTop / qSlices : 1.0;
   double phaseSum = 0.0, phaseZ = 0.0;
   phase_.assign(N * slices_, 0.0f);
   atten_.clear();
   if (spec.absorptionPerUm > 0.0)
      atten_.assign(N * slices_, 1.0f);
   if (zTop > 0.0)
   {
      const int group = std::max(1, std::min(qSlices, static_cast<int>(kVolumeQueryFloats / (3.0 * N))));
      std::vector<float> vol;
      const double dnC = spec.nCytoplasm - spec.nMedium, dnN = spec.nNucleus - spec.nMedium;
      const double dnM = spec.nMicrotubule - spec.nCytoplasm; // a tube displaces cytoplasm
      for (int g0 = 0; g0 < qSlices; g0 += group)
      {
         const int G = std::min(group, qSlices - g0);
         vol.assign(3 * N * G, 0.0f);
         const double zMax = g0 + G == qSlices ? zTop : (g0 + G) * qDz;
         if (src.OpticalVolume(gx0, gy0, gx1, gy1, g0 * qDz, zMax, nx_, ny_, G, std::max(1, q.sub), vol.data()) < 0)
         {
            err = "BrightField: the optical volume query failed.";
            return false;
         }
         const size_t chan = N * G;
         for (int k = 0; k < G; ++k)
            for (size_t i = 0; i < N; ++i)
            {
               const size_t v = static_cast<size_t>(k) * N + i;
               const double fc = vol[v], fn = vol[chan + v], fm = vol[2 * chan + v];
               const double ph = k0_ * qDz * (fc * dnC + fn * dnN + fm * dnM);
               const size_t o = (slices_ == 1 ? 0 : static_cast<size_t>(g0 + k) * N) + i;
               phase_[o] = static_cast<float>(phase_[o] + ph);
               if (slices_ == 1)
               {
                  phaseSum += std::fabs(ph);
                  phaseZ += std::fabs(ph) * (g0 + k + 0.5) * qDz;
               }
               if (!atten_.empty())
                  atten_[o] = static_cast<float>(atten_[o] * std::exp(-0.5 * spec.absorptionPerUm * qDz * (fc + fn)));
            }
      }
   }
   if (slices_ == 1 && phaseSum > 0.0)
      objectZ_ = phaseZ / phaseSum;
   // The grid is periodic: cells cut by its edge would wrap with a phase
   // jump and ring into the FOV. Fade the specimen out over the outer half
   // of the margin (cosine taper), so the wrap is seamless.
   {
      const unsigned t = margin_ / 2;
      auto ramp = [&](unsigned i, unsigned n) {
         const unsigned d = std::min(i, n - 1 - i);
         return d >= t ? 1.0 : 0.5 - 0.5 * std::cos(kPi * (d + 0.5) / std::max(1u, t));
      };
      if (t > 0)
         for (unsigned y = 0; y < ny_; ++y)
         {
            const double wy = ramp(y, ny_);
            for (unsigned x = 0; x < nx_; ++x)
            {
               const double wgt = wy * ramp(x, nx_);
               if (wgt >= 1.0)
                  continue;
               for (int k = 0; k < slices_; ++k)
               {
                  const size_t i = static_cast<size_t>(k) * N + static_cast<size_t>(y) * nx_ + x;
                  phase_[i] = static_cast<float>(phase_[i] * wgt);
                  if (!atten_.empty())
                     atten_[i] = static_cast<float>(std::pow(static_cast<double>(atten_[i]), wgt));
               }
            }
         }
   }
   Finish();
   srcId_ = &src;
   worldVersion_ = worldVersion;
   setupMs_ = Ms(t0);
   return true;
}

bool BrightfieldScene::UpdateFromPhase(const BrightfieldSpec& spec, int slices, double zTopUm,
                                       const std::vector<float>& phase, std::string& err)
{
   const auto t0 = std::chrono::steady_clock::now();
   if (!Begin(spec, err))
      return false;
   if (slices < 1 || !(zTopUm > 0) || phase.size() != static_cast<size_t>(nx_) * ny_ * slices)
   {
      err = "BrightField: bad phase slices.";
      return false;
   }
   slices_ = slices;
   dz_ = zTopUm / slices;
   objectZ_ = 0.5 * dz_;
   phase_ = phase;
   atten_.clear();
   Finish();
   srcId_ = nullptr;
   setupMs_ = Ms(t0);
   return true;
}

// Sources, pupil, kz, and the thin spectrum or the exit spectra.
void BrightfieldScene::Finish()
{
   const BrightfieldQuality q = spec_.Resolved();
   const size_t N = static_cast<size_t>(nx_) * ny_;
   const BrightfieldSpec& spec = spec_;

   // Condenser source points: equal-area Fibonacci disk, snapped to the grid.
   src_.clear();
   const int ns = std::max(1, q.sources);
   const double Lx = nx_ * pitch_, Ly = ny_ * pitch_;
   for (int i = 0; i < ns; ++i)
   {
      double kx = 0, ky = 0;
      if (ns > 1 && spec.condenserNa > 0)
      {
         const double r = spec.condenserNa * std::sqrt((i + 0.5) / ns), th = i * 2.399963229728653;
         kx = k0_ * r * std::cos(th);
         ky = k0_ * r * std::sin(th);
      }
      src_.push_back({static_cast<int>(std::lround(kx * Lx / (2 * kPi))), static_cast<int>(std::lround(ky * Ly / (2 * kPi)))});
   }

   // FFT column sets: all, and the propagating band |kx| < k0 n_medium.
   allCols_.resize(nx_);
   bandCols_.clear();
   for (unsigned ix = 0; ix < nx_; ++ix)
   {
      allCols_[ix] = ix;
      if (std::fabs(2 * kPi * Freq(ix, nx_) / Lx) < k0_ * spec.nMedium)
         bandCols_.push_back(ix);
   }

   // Detection pupil and the medium's kz (rows in parallel: per element).
   pupil_.assign(N, cfloat(0, 0));
   kz_.assign(N, -1.0f);
   const double kMed = k0_ * spec.nMedium, kNa = k0_ * spec.na;
   const ZernikeModeTable zmodes(spec.zernike);
   ParallelFor(ny_, [&](unsigned iy) {
      for (unsigned ix = 0; ix < nx_; ++ix)
      {
         const double kx = 2 * kPi * Freq(ix, nx_) / Lx, ky = 2 * kPi * Freq(iy, ny_) / Ly;
         const double kr2 = kx * kx + ky * ky;
         const size_t i = static_cast<size_t>(iy) * nx_ + ix;
         if (kr2 < kMed * kMed)
            kz_[i] = static_cast<float>(std::sqrt(kMed * kMed - kr2));
         const double rho = std::sqrt(kr2) / kNa;
         if (rho <= 1.0 && kz_[i] >= 0)
         {
            const double w = 2 * kPi * zmodes.Waves(rho, std::atan2(ky, kx));
            pupil_[i] = cfloat(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
         }
      }
   });

   // Thin object: one transmittance spectrum that every source shifts.
   thinSpec_.clear();
   exit_.clear();
   prop_.clear();
   trans_.clear();
   if (slices_ == 1)
   {
      thinSpec_.resize(N);
      for (size_t i = 0; i < N; ++i)
      {
         const float a = atten_.empty() ? 1.0f : atten_[i];
         thinSpec_[i] = std::polar(a, phase_[i]);
      }
      std::vector<cfloat> work;
      FftForward(thinSpec_.data(), work, false);
   }
   else
   {
      // Shared by every source: the slice step and the slice transmittances
      // (per element; rows in parallel).
      prop_.resize(N);
      ParallelFor(ny_, [&](unsigned y) {
         for (size_t i = static_cast<size_t>(y) * nx_; i < static_cast<size_t>(y + 1) * nx_; ++i)
            prop_[i] = kz_[i] >= 0 ? std::polar(1.0f, static_cast<float>(kz_[i] * dz_)) : cfloat(0, 0);
      });
      trans_.clear();
      if (static_cast<double>(N) * slices_ * sizeof(cfloat) <= kTransCacheBytes)
      {
         trans_.resize(N * slices_);
         ParallelFor(static_cast<unsigned>(slices_) * ny_, [&](unsigned r) {
            for (size_t i = static_cast<size_t>(r) * nx_; i < static_cast<size_t>(r + 1) * nx_; ++i)
               trans_[i] = std::polar(atten_.empty() ? 1.0f : atten_[i], phase_[i]);
         });
      }
   }
   if (slices_ > 1 && static_cast<double>(src_.size()) * N * sizeof(cfloat) <= kExitCacheBytes)
   {
      exit_.resize(src_.size());
      ParallelFor(static_cast<unsigned>(src_.size()), [&](unsigned s) {
         std::vector<cfloat> work;
         ExitField(src_[s], exit_[s], work);
      });
   }
   valid_ = true;
}

// Propagates source s's plane wave down through the slices (top first):
// screen, then dz to the next screen. Leaves the spectrum just below the
// lowest screen (its mid-plane) in u.
void BrightfieldScene::ExitField(const Source& s, std::vector<cfloat>& u, std::vector<cfloat>& work) const
{
   const size_t N = static_cast<size_t>(nx_) * ny_;
   u.resize(N);
   // The tilted plane wave, separable in x and y.
   std::vector<cfloat> ex(nx_);
   for (unsigned x = 0; x < nx_; ++x)
      ex[x] = std::polar(1.0f, static_cast<float>(2 * kPi * static_cast<double>(s.mx) * x / nx_));
   for (unsigned y = 0; y < ny_; ++y)
   {
      const cfloat ey = std::polar(1.0f, static_cast<float>(2 * kPi * static_cast<double>(s.my) * y / ny_));
      cfloat* row = &u[static_cast<size_t>(y) * nx_];
      for (unsigned x = 0; x < nx_; ++x)
         row[x] = ex[x] * ey;
   }
   for (int k = slices_ - 1; k >= 0; --k)
   {
      if (!trans_.empty())
         MulInPlace(u.data(), &trans_[static_cast<size_t>(k) * N], N);
      else
      {
         const float* ph = &phase_[static_cast<size_t>(k) * N];
         const float* at = atten_.empty() ? nullptr : &atten_[static_cast<size_t>(k) * N];
         for (size_t i = 0; i < N; ++i)
            u[i] *= std::polar(at ? at[i] : 1.0f, ph[i]);
      }
      // Only the propagating band is needed: prop_ (and later the pupil)
      // is zero elsewhere.
      FftForward(u.data(), work, true);
      if (k == 0)
         break;
      MulInPlace(u.data(), prop_.data(), N);
      FftInverse(u.data(), work, 0, ny_);
   }
}

void BrightfieldScene::SourceImage(int s, const std::vector<cfloat>& defocus, std::vector<cfloat>& u,
                                   std::vector<cfloat>& work, float* camOut) const
{
   const size_t N = static_cast<size_t>(nx_) * ny_;
   const Source& sp = src_[s];
   if (slices_ == 1)
   {
      // E(k) = T(k - k_s): a circular shift of the transmittance spectrum
      // (the shifted column index tabulated once per source).
      u.resize(N);
      std::vector<unsigned> sxs(nx_);
      for (unsigned x = 0; x < nx_; ++x)
         sxs[x] = static_cast<unsigned>(((static_cast<int>(x) - sp.mx) % static_cast<int>(nx_) + static_cast<int>(nx_)) % static_cast<int>(nx_));
      for (unsigned y = 0; y < ny_; ++y)
      {
         const unsigned sy = static_cast<unsigned>(((static_cast<int>(y) - sp.my) % static_cast<int>(ny_) + static_cast<int>(ny_)) % static_cast<int>(ny_));
         const cfloat* src = &thinSpec_[static_cast<size_t>(sy) * nx_];
         cfloat* dst = &u[static_cast<size_t>(y) * nx_];
         for (unsigned x = 0; x < nx_; ++x)
            dst[x] = src[sxs[x]];
      }
   }
   else if (!exit_.empty())
      u = exit_[s];
   else
      ExitField(sp, u, work);
   // Pupil and defocus to the focal plane (Image), zero outside the band.
   MulInPlace(u.data(), defocus.data(), N);
   const unsigned W = spec_.width, H = spec_.height;
   FftInverse(u.data(), work, margin_, margin_ + H * up_);
   const float norm = 1.0f / static_cast<float>(up_ * up_);
   for (unsigned j = 0; j < H; ++j)
      for (unsigned i = 0; i < W; ++i)
      {
         float acc = 0.0f;
         for (unsigned b = 0; b < up_; ++b)
            for (unsigned a = 0; a < up_; ++a)
            {
               const cfloat v = u[static_cast<size_t>(margin_ + j * up_ + b) * nx_ + margin_ + i * up_ + a];
               acc += v.real() * v.real() + v.imag() * v.imag();   // std::norm's operations
            }
         camOut[static_cast<size_t>(j) * W + i] = acc * norm;
      }
}

bool BrightfieldScene::Image(double focusUm, std::vector<float>& out, std::string& err)
{
   if (!valid_)
   {
      err = "BrightField: scene not set up.";
      return false;
   }
   if (haveImage_ && imageFocus_ == focusUm)
   {
      out = image_;
      return true;
   }
   const auto t0 = std::chrono::steady_clock::now();
   const size_t P = static_cast<size_t>(spec_.width) * spec_.height;
   // From the lowest screen (objectZ_ above the coverslip: its slice's
   // mid-plane, or the thin screen's height) to the focal plane, travelling
   // down: distance objectZ_ - focus. Shared by every source.
   const size_t N = static_cast<size_t>(nx_) * ny_;
   const double d = objectZ_ - focusUm;
   std::vector<cfloat> defocus(N);
   ParallelFor(ny_, [&](unsigned y) {
      for (size_t i = static_cast<size_t>(y) * nx_; i < static_cast<size_t>(y + 1) * nx_; ++i)
         defocus[i] = kz_[i] >= 0 ? pupil_[i] * std::polar(1.0f, static_cast<float>(kz_[i] * d)) : cfloat(0, 0);
   });
   std::vector<float> slots(P * src_.size());
   ParallelFor(static_cast<unsigned>(src_.size()), [&](unsigned s) {
      std::vector<cfloat> u, work;
      SourceImage(static_cast<int>(s), defocus, u, work, &slots[s * P]);
   });
   image_.assign(P, 0.0f);
   for (size_t s = 0; s < src_.size(); ++s)
      for (size_t i = 0; i < P; ++i)
         image_[i] += slots[s * P + i];
   const float inv = 1.0f / static_cast<float>(src_.size());
   for (float& v : image_)
      v *= inv;
   haveImage_ = true;
   imageFocus_ = focusUm;
   imageMs_ = Ms(t0);
   out = image_;
   return true;
}

} // namespace sim
