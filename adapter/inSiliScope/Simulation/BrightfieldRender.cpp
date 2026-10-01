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
      {6, 1, 1, 0.0, 3.0},    // 1: thin object, few sources, camera-pixel grid
      {12, 1, 1, 1.0, 3.0},   // 2
      {24, 2, 2, 0.5, 4.0},   // 3: default
      {48, 2, 3, 0.25, 5.0},  // 4
      {96, 3, 4, 0.125, 6.0}, // 5: slow
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

void BrightfieldScene::Fft2(cfloat* a, std::vector<cfloat>& work, bool inverse) const
{
   const size_t n = static_cast<size_t>(nx_) * ny_;
   work.resize(n);
   if (inverse)
      for (size_t i = 0; i < n; ++i)
         a[i] = std::conj(a[i]);
   // Along y: element y of transform x sits at a[y * nx + x].
   cfloat* r = planY_.Forward(a, work.data(), nx_);
   if (r != a)
      std::copy(work.begin(), work.end(), a);
   // Along x, on the transpose.
   for (unsigned y = 0; y < ny_; ++y)
      for (unsigned x = 0; x < nx_; ++x)
         work[static_cast<size_t>(x) * ny_ + y] = a[static_cast<size_t>(y) * nx_ + x];
   r = planX_.Forward(work.data(), a, ny_);
   const cfloat* t = r;
   if (r == a)
   {
      std::copy(a, a + n, work.begin());
      t = work.data();
   }
   for (unsigned y = 0; y < ny_; ++y)
      for (unsigned x = 0; x < nx_; ++x)
         a[static_cast<size_t>(y) * nx_ + x] = t[static_cast<size_t>(x) * ny_ + y];
   if (inverse)
   {
      const float s = 1.0f / static_cast<float>(n);
      for (size_t i = 0; i < n; ++i)
         a[i] = std::conj(a[i]) * s;
   }
}

void BrightfieldScene::GridFor(const BrightfieldSpec& spec, unsigned& nx, unsigned& ny, unsigned& marginCells)
{
   const BrightfieldQuality q = spec.Resolved();
   const unsigned up = static_cast<unsigned>(std::max(1, q.upscale));
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
   const BrightfieldQuality q = spec.Resolved();
   up_ = static_cast<unsigned>(std::max(1, q.upscale));
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

   // Detection pupil and the medium's kz.
   pupil_.assign(N, cfloat(0, 0));
   kz_.assign(N, -1.0f);
   const double kMed = k0_ * spec.nMedium, kNa = k0_ * spec.na;
   for (unsigned iy = 0; iy < ny_; ++iy)
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
            const double w = 2 * kPi * ZernikeWavefrontWaves(spec.zernike, rho, std::atan2(ky, kx));
            pupil_[i] = cfloat(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
         }
      }

   // Thin object: one transmittance spectrum that every source shifts.
   thinSpec_.clear();
   exit_.clear();
   if (slices_ == 1)
   {
      thinSpec_.resize(N);
      for (size_t i = 0; i < N; ++i)
      {
         const float a = atten_.empty() ? 1.0f : atten_[i];
         thinSpec_[i] = std::polar(a, phase_[i]);
      }
      std::vector<cfloat> work;
      Fft2(thinSpec_.data(), work, false);
   }
   else if (static_cast<double>(src_.size()) * N * sizeof(cfloat) <= kExitCacheBytes)
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
   for (unsigned y = 0; y < ny_; ++y)
      for (unsigned x = 0; x < nx_; ++x)
      {
         const double ph = 2 * kPi * (static_cast<double>(s.mx) * x / nx_ + static_cast<double>(s.my) * y / ny_);
         u[static_cast<size_t>(y) * nx_ + x] = std::polar(1.0f, static_cast<float>(ph));
      }
   std::vector<cfloat> prop(N);
   for (size_t i = 0; i < N; ++i)
      prop[i] = kz_[i] >= 0 ? std::polar(1.0f, static_cast<float>(kz_[i] * dz_)) : cfloat(0, 0);
   for (int k = slices_ - 1; k >= 0; --k)
   {
      const float* ph = &phase_[static_cast<size_t>(k) * N];
      const float* at = atten_.empty() ? nullptr : &atten_[static_cast<size_t>(k) * N];
      for (size_t i = 0; i < N; ++i)
         u[i] *= std::polar(at ? at[i] : 1.0f, ph[i]);
      Fft2(u.data(), work, false);
      if (k == 0)
         break;
      for (size_t i = 0; i < N; ++i)
         u[i] *= prop[i];
      Fft2(u.data(), work, true);
   }
}

void BrightfieldScene::SourceImage(int s, double focusUm, std::vector<cfloat>& u, std::vector<cfloat>& work,
                                   float* camOut) const
{
   const size_t N = static_cast<size_t>(nx_) * ny_;
   const Source& sp = src_[s];
   if (slices_ == 1)
   {
      // E(k) = T(k - k_s): a circular shift of the transmittance spectrum.
      u.resize(N);
      for (unsigned y = 0; y < ny_; ++y)
      {
         const unsigned sy = static_cast<unsigned>(((static_cast<int>(y) - sp.my) % static_cast<int>(ny_) + static_cast<int>(ny_)) % static_cast<int>(ny_));
         for (unsigned x = 0; x < nx_; ++x)
         {
            const unsigned sx = static_cast<unsigned>(((static_cast<int>(x) - sp.mx) % static_cast<int>(nx_) + static_cast<int>(nx_)) % static_cast<int>(nx_));
            u[static_cast<size_t>(y) * nx_ + x] = thinSpec_[static_cast<size_t>(sy) * nx_ + sx];
         }
      }
   }
   else if (!exit_.empty())
      u = exit_[s];
   else
      ExitField(sp, u, work);
   // From the lowest screen (objectZ_ above the coverslip: its slice's
   // mid-plane, or the thin screen's height) to the focal plane, travelling
   // down: distance objectZ_ - focus.
   const double d = objectZ_ - focusUm;
   for (size_t i = 0; i < N; ++i)
      u[i] = kz_[i] >= 0 ? u[i] * pupil_[i] * std::polar(1.0f, static_cast<float>(kz_[i] * d)) : cfloat(0, 0);
   Fft2(u.data(), work, true);
   const unsigned W = spec_.width, H = spec_.height;
   const float norm = 1.0f / static_cast<float>(up_ * up_);
   for (unsigned j = 0; j < H; ++j)
      for (unsigned i = 0; i < W; ++i)
      {
         float acc = 0.0f;
         for (unsigned b = 0; b < up_; ++b)
            for (unsigned a = 0; a < up_; ++a)
               acc += std::norm(u[static_cast<size_t>(margin_ + j * up_ + b) * nx_ + margin_ + i * up_ + a]);
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
   std::vector<float> slots(P * src_.size());
   ParallelFor(static_cast<unsigned>(src_.size()), [&](unsigned s) {
      std::vector<cfloat> u, work;
      SourceImage(static_cast<int>(s), focusUm, u, work, &slots[s * P]);
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
