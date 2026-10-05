// ctest widefield: the WideField modality's pieces (WidefieldRender.h,
// Fft2d.h, Illumination.h) against direct references.
//
//   widefield_check          exit code 0 = all checks passed
#include "CellFieldSource.h"
#include "Fft2d.h"
#include "Illumination.h"
#include "ScopeMovie.h"
#include "WidefieldRender.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;
const double kPi = 3.14159265358979323846;

void Check(bool ok, const char* what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok) g_failures++;
}

double Sum(const std::vector<float>& v)
{
   double s = 0.0;
   for (float x : v) s += x;
   return s;
}

double MaxRelDiff(const std::vector<float>& a, const std::vector<float>& b)
{
   if (a.size() != b.size()) return 1e30;
   double m = 0.0, peak = 0.0;
   for (size_t i = 0; i < a.size(); i++) {
      m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
      peak = std::max(peak, std::fabs(static_cast<double>(b[i])));
   }
   return peak > 0 ? m / peak : m;
}

double RmsRelDiff(const std::vector<float>& a, const std::vector<float>& b)
{
   if (a.size() != b.size()) return 1e30;
   double e = 0.0, s = 0.0;
   for (size_t i = 0; i < a.size(); i++) {
      e += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
      s += static_cast<double>(b[i]) * b[i];
   }
   return s > 0 ? std::sqrt(e / s) : std::sqrt(e);
}

// A PSF whose width depends on z, so the plane blending is exercised.
class TestPsf : public WidefieldPsf
{
public:
   TestPsf(double pitch, double s0 = 0.12, double slope = 0.02, int R = 6) : pitch_(pitch), s0_(s0), slope_(slope), R_(R) {}
   double PlaneCoord(double defocusUm) const override { return defocusUm / 0.1; }
   int MinPlane() const override { return -30; }
   int MaxPlane() const override { return 30; }
   int Radius(int, int) const override { return R_; }
   double SigmaCells(int p) const { return (s0_ + slope_ * std::abs(p)) / pitch_; }
   double NormSum(int p, int R) const
   {
      const double s = SigmaCells(p);
      double sum = 0;
      for (int y = -R; y <= R; y++)
         for (int x = -R; x <= R; x++) sum += std::exp(-(x * x + y * y) / (2 * s * s));
      return sum;
   }
   void Kernel(int p, int R, std::vector<float>& out) const override
   {
      const int D = 2 * R + 1;
      const double s = SigmaCells(p), sum = NormSum(p, R);
      out.assign(static_cast<size_t>(D) * D, 0.0f);
      for (int y = -R; y <= R; y++)
         for (int x = -R; x <= R; x++)
            out[static_cast<size_t>(y + R) * D + x + R] = static_cast<float>(std::exp(-(x * x + y * y) / (2 * s * s)) / sum);
   }

private:
   double pitch_, s0_, slope_;
   int R_;
};

// Scalar-diffraction defocused widefield PSF (circular pupil, NA 1.4, oil,
// 670 nm): the realistic spectrum the focus bands are judged on. Planes every
// 0.1 um over +/- 3 um.
class ScalarPsf : public WidefieldPsf
{
public:
   explicit ScalarPsf(double pitch, int R = 40) : pitch_(pitch), R_(R)
   {
      const unsigned M = 256;
      FftPlan1d plan(M);
      const double lambda = 0.67, na = 1.4, n = 1.518;
      for (int p = 0; p <= 60; p++) {
         const double z = (p - 30) * 0.1;
         std::vector<std::complex<float>> f(M * M), w(M * M);
         for (unsigned iy = 0; iy < M; iy++)
            for (unsigned ix = 0; ix < M; ix++) {
               const double kx = (ix <= M / 2 ? double(ix) : double(ix) - M) / (M * pitch);
               const double ky = (iy <= M / 2 ? double(iy) : double(iy) - M) / (M * pitch);
               const double k2 = kx * kx + ky * ky;
               if (k2 > (na / lambda) * (na / lambda)) continue;
               const double ph = 2 * kPi * z * std::sqrt((n / lambda) * (n / lambda) - k2);
               f[iy * M + ix] = std::complex<float>((float)std::cos(ph), (float)std::sin(ph));
            }
         // 2D FFT: rows then columns (B = 1).
         for (unsigned y = 0; y < M; y++) {
            std::vector<cfloat> a(f.begin() + y * M, f.begin() + (y + 1) * M), t(M);
            const cfloat* r = plan.Forward(a.data(), t.data(), 1);
            std::copy(r, r + M, f.begin() + y * M);
         }
         for (unsigned x = 0; x < M; x++) {
            std::vector<cfloat> a(M), t(M);
            for (unsigned y = 0; y < M; y++) a[y] = f[y * M + x];
            const cfloat* r = plan.Forward(a.data(), t.data(), 1);
            for (unsigned y = 0; y < M; y++) f[y * M + x] = r[y];
         }
         const int D = 2 * R_ + 1;
         std::vector<double> k(D * D);
         double sum = 0;
         for (int dy = -R_; dy <= R_; dy++)
            for (int dx = -R_; dx <= R_; dx++) {
               const double v = std::norm(f[((dy + M) % M) * M + (dx + M) % M]);
               k[(dy + R_) * D + dx + R_] = v;
               sum += v;
            }
         std::vector<float> kf(D * D);
         for (size_t i = 0; i < k.size(); i++) kf[i] = (float)(k[i] / sum);
         planes_.push_back(kf);
      }
   }
   double PlaneCoord(double defocusUm) const override { return defocusUm / 0.1 + 30; }
   int MinPlane() const override { return 0; }
   int MaxPlane() const override { return 60; }
   int Radius(int, int) const override { return R_; }
   void Kernel(int p, int R, std::vector<float>& out) const override
   {
      const int D = 2 * R + 1, DS = 2 * R_ + 1;
      out.assign(static_cast<size_t>(D) * D, 0.0f);
      for (int dy = -std::min(R, R_); dy <= std::min(R, R_); dy++)
         for (int dx = -std::min(R, R_); dx <= std::min(R, R_); dx++)
            out[(dy + R) * D + dx + R] = planes_[p][(dy + R_) * DS + dx + R_];
   }

private:
   double pitch_;
   int R_;
   std::vector<std::vector<float>> planes_;
};

// Two excitation levels: 1 on the left half of the square, 0.5 on the right.
class TwoLevelIllumination : public IlluminationPattern
{
public:
   TwoLevelIllumination(double w, double h) : w_(w), h_(h) {}
   double At(double x, double y) const override
   {
      if (x < -w_ / 2 || x >= w_ / 2 || y < -h_ / 2 || y >= h_ / 2) return 0.0;
      return x < 0 ? 1.0 : 0.5;
   }
   void Support(double& x0, double& y0, double& x1, double& y1) const override
   {
      x0 = -w_ / 2; x1 = w_ / 2; y0 = -h_ / 2; y1 = h_ / 2;
   }

private:
   double w_, h_;
};

// A smooth (Gaussian) beam: many distinct frame doses.
class BeamIllumination : public IlluminationPattern
{
public:
   BeamIllumination(double w, double sigma) : w_(w), s_(sigma) {}
   double At(double x, double y) const override
   {
      if (std::fabs(x) >= w_ / 2 || std::fabs(y) >= w_ / 2) return 0.0;
      return std::exp(-(x * x + y * y) / (2 * s_ * s_));
   }
   void Support(double& x0, double& y0, double& x1, double& y1) const override
   {
      x0 = y0 = -w_ / 2; x1 = y1 = w_ / 2;
   }

private:
   double w_, s_;
};

// pixel 0.1 um; origin chosen so the FOV corner sits on a cell boundary
// (no sub-cell shift) unless frac is given.
WidefieldSceneSpec BaseSpec(unsigned W, int upscale, double frac = 0.0)
{
   WidefieldSceneSpec s;
   const double pitch = 0.1 / upscale;
   s.originXUm = 3.0 + 0.05 + frac * pitch;
   s.originYUm = -2.0 + 0.05 + frac * pitch;
   s.width = s.height = W;
   s.pixelUm = 0.1;
   s.focusWorldUm = 1.0;
   s.slabCentreUm = 1.0;
   s.slabHalfUm = 1.0;
   s.grid.upscale = upscale;
   s.grid.zPlaneNm = 25;
   s.eta = WidefieldCollectionEfficiency(1.4, 1.518);
   s.exposureSec = 0.05;
   return s;
}

void FftVsDft()
{
   bool ok = true;
   double worst = 0, worstRt = 0;
   for (auto sz : std::vector<std::pair<unsigned, unsigned>>{{12, 10}, {30, 18}, {40, 45}, {16, 16}, {24, 50}}) {
      const unsigned nx = sz.first, ny = sz.second, iw = nx - 3, ih = ny - 2;
      RealFft2d f(nx, ny);
      std::mt19937 rng(nx * 131 + ny);
      std::uniform_real_distribution<float> U(-1, 1);
      std::vector<float> x(iw * ih);
      for (auto& v : x) v = U(rng);
      std::vector<cfloat> S(f.SpecSize());
      f.Forward(x.data(), iw, ih, iw, S.data());
      double err = 0, peak = 0;
      for (unsigned ky = 0; ky < ny; ky++)
         for (unsigned kx = 0; kx <= nx / 2; kx++) {
            std::complex<double> s = 0;
            for (unsigned y = 0; y < ih; y++)
               for (unsigned xx = 0; xx < iw; xx++)
                  s += double(x[y * iw + xx]) * std::polar(1.0, -2 * kPi * (double(kx * xx) / nx + double(ky * y) / ny));
            err = std::max(err, std::abs(s - std::complex<double>(S[ky * f.SpecW() + kx])));
            peak = std::max(peak, std::abs(s));
         }
      std::vector<float> o(nx * ny);
      f.Inverse(S.data(), o.data(), 0, ny, 0, nx, nx);
      double rt = 0;
      for (unsigned y = 0; y < ny; y++)
         for (unsigned xx = 0; xx < nx; xx++)
            rt = std::max(rt, (double)std::fabs(o[y * nx + xx] - ((y < ih && xx < iw) ? x[y * iw + xx] : 0.0f)));
      worst = std::max(worst, err / peak);
      worstRt = std::max(worstRt, rt);
      ok = ok && err / peak < 1e-5 && rt < 1e-5;
   }
   char msg[160];
   std::snprintf(msg, sizeof msg, "real FFT (mixed radix 2/3/4/5) vs naive DFT: rel err %.1e, round trip %.1e", worst, worstRt);
   Check(ok, msg);
   Check(RealFft2d::FastSize(300, 8) == 320 && RealFft2d::FastSize(97, 2) == 100 && RealFft2d::FastSize(7, 1) == 8,
         "FastSize picks 2^a 3^b 5^c");
}

// Synthetic dye grid: random counts on planes around the focus.
WidefieldDyeGrid RandomGrid(const WidefieldGridSpec& g, bool bleach, bool persist, unsigned seed, double z0 = 0.6,
                            unsigned nz = 32, int sparsity = 30)
{
   WidefieldDyeGrid G;
   G.spec = g;
   G.k0 = static_cast<long>(std::floor(z0 / g.zPlaneUm));
   G.nz = nz;
   const size_t n = static_cast<size_t>(g.nx) * g.ny * G.nz;
   std::mt19937 rng(seed);
   std::uniform_int_distribution<int> D(0, sparsity);
   if (bleach) G.bleaching.resize(n);
   if (persist) G.persistent.resize(n);
   for (size_t i = 0; i < n; i++) {
      const bool on = D(rng) == 0;
      if (bleach) { G.bleaching[i] = on ? (float)(1 + D(rng) % 3) : 0.0f; G.nBleaching += (long)G.bleaching[i]; }
      if (persist) { G.persistent[i] = D(rng) == 1 ? 1.0f : 0.0f; G.nPersistent += (long)G.persistent[i]; }
   }
   return G;
}

// Direct convolution reference: each dye plane (inside the slab) to PSF
// planes p0, p0+1 with linear weights, convolve, crop the FOV, clamp, bin.
std::vector<float> DirectImage(const WidefieldDyeGrid& G, const WidefieldSceneSpec& s, const WidefieldPsf& psf,
                               const std::vector<float>& wb, const std::vector<float>& wp, unsigned fx, unsigned fy,
                               int R)
{
   const WidefieldGridSpec& g = G.spec;
   const size_t n2 = static_cast<size_t>(g.nx) * g.ny;
   const int u = s.grid.upscale, D = 2 * R + 1;
   std::vector<double> img(n2, 0.0);
   std::vector<float> k;
   const double dz = g.zPlaneUm;
   for (unsigned i = 0; i < G.nz; i++) {
      const long kk = G.k0 + (long)i;
      if (s.slabHalfUm > 0 && ((kk + 1) * dz <= s.slabCentreUm - s.slabHalfUm + 1e-12 ||
                               kk * dz >= s.slabCentreUm + s.slabHalfUm - 1e-12))
         continue;
      double t = psf.PlaneCoord(G.PlaneCentreUm(i) - s.focusWorldUm);
      t = std::min<double>(psf.MaxPlane(), std::max<double>(psf.MinPlane(), t));
      int p0 = (int)std::floor(t);
      double fr = t - p0;
      if (p0 >= psf.MaxPlane()) { p0 = psf.MaxPlane(); fr = 0; }
      for (int sIdx = 0; sIdx < 2; sIdx++) {
         const double w = sIdx ? fr : 1 - fr;
         if (w == 0) continue;
         psf.Kernel(p0 + sIdx, R, k);
         for (unsigned iy = 0; iy < g.ny; iy++)
            for (unsigned ix = 0; ix < g.nx; ix++) {
               const size_t c = (size_t)iy * g.nx + ix;
               double a = 0;
               if (!G.bleaching.empty()) a += (double)G.bleaching[i * n2 + c] * wb[c];
               if (!G.persistent.empty()) a += (double)G.persistent[i * n2 + c] * wp[c];
               if (a == 0) continue;
               for (int dy = -R; dy <= R; dy++)
                  for (int dx = -R; dx <= R; dx++) {
                     const int X = (int)ix + dx, Y = (int)iy + dy;
                     if (X < 0 || Y < 0 || X >= (int)g.nx || Y >= (int)g.ny) continue;
                     img[(size_t)Y * g.nx + X] += w * a * k[(size_t)(dy + R) * D + dx + R];
                  }
            }
      }
   }
   std::vector<float> cam((size_t)s.width * s.height, 0.0f);
   for (unsigned Y = 0; Y < s.height; Y++)
      for (unsigned X = 0; X < s.width; X++) {
         double acc = 0;
         for (int sy = 0; sy < u; sy++)
            for (int sx = 0; sx < u; sx++) acc += std::max(0.0, img[(size_t)(fy + Y * u + sy) * g.nx + fx + X * u + sx]);
         cam[(size_t)Y * s.width + X] = (float)acc;
      }
   return cam;
}

std::vector<float> Wp(const WidefieldScene& sc, const WidefieldSceneSpec& s)
{
   std::vector<float> wp(sc.FrameDose().size());
   for (size_t i = 0; i < wp.size(); i++) wp[i] = (float)(s.eta * sc.FrameDose()[i]);
   return wp;
}

void ConvolutionVsDirect()
{
   for (int u : {1, 2}) {
      WidefieldSceneSpec s = BaseSpec(24, u);
      // Illumination wider than the FOV: exercises the margin.
      SquareIllumination ill(3.0, 2.8);
      unsigned fx = 0, fy = 0;
      double frx = 1, fry = 1;
      const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s, &fx, &fy, &frx, &fry);
      TestPsf psf(g.pitchUm);
      WidefieldDyeGrid G = RandomGrid(g, true, true, 11 + u);
      WidefieldScene sc;
      std::string err;
      const bool ok = sc.UpdateFromGrid(G, ill, s, psf, err);
      std::vector<float> wb, cam;
      sc.FreshBleachWeights(3, wb);
      sc.RenderFrame(wb, cam);
      const std::vector<float> ref = DirectImage(G, s, psf, wb, Wp(sc, s), fx, fy, sc.KernelRadius());
      const double d = MaxRelDiff(cam, ref);
      char msg[200];
      std::snprintf(msg, sizeof msg,
                    "FFT convolution == direct convolution, upscale %d, FOV cell (%u, %u), frac %.2g (max rel diff %.2e)%s",
                    u, fx, fy, frx + fry, d, ok ? "" : err.c_str());
      Check(ok && fx > 0 && frx == 0 && fry == 0 && d < 1e-4, msg);
   }
}

// Focus bands: a realistic defocused PSF, dyes over +/- 2.5 um; the image
// against the full-resolution direct convolution. At the real criterion no
// plane of a sharp-pupil PSF goes coarse (its defocused disk's rim keeps
// energy up to the NA cutoff); a loosened one exercises the coarse levels.
void FocusBands()
{
   for (double eps : {WidefieldScene::kBandEpsilon, 2e-2}) {
      WidefieldSceneSpec s = BaseSpec(40, 1);
      s.slabHalfUm = 3.0;
      SquareIllumination ill(4.0, 4.0);
      unsigned fx = 0, fy = 0;
      const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s, &fx, &fy);
      ScalarPsf psf(g.pitchUm);
      WidefieldDyeGrid G = RandomGrid(g, false, true, 21, s.focusWorldUm - 2.5, 200, 60);
      WidefieldScene sc;
      sc.SetBandEpsilonForTesting(eps);
      std::string err;
      const bool ok = sc.UpdateFromGrid(G, ill, s, psf, err);
      std::vector<float> wb, cam;
      sc.FreshBleachWeights(0, wb);
      sc.RenderFrame(wb, cam);
      const std::vector<float> ref = DirectImage(G, s, psf, wb, Wp(sc, s), fx, fy, sc.KernelRadius());
      const unsigned* lv = sc.PlanesPerLevel();
      const double rms = RmsRelDiff(cam, ref), mx = MaxRelDiff(cam, ref);
      char msg[260];
      std::snprintf(msg, sizeof msg,
                    "focus bands (criterion %.0e): %u/%u/%u dye planes at full/half/quarter resolution, %ux%u FFT, R %d; "
                    "vs direct: rms %.1e, max %.1e of peak",
                    eps, lv[0], lv[1], lv[2], sc.FftSizeX(), sc.FftSizeY(), sc.KernelRadius(), rms, mx);
      if (eps == WidefieldScene::kBandEpsilon)
         Check(ok && lv[1] + lv[2] == 0 && rms < 1e-5, msg);
      else
         Check(ok && lv[1] > 0 && lv[2] > 0 && rms < 1e-2, msg);
   }
}

// Sub-cell stage positions: a band-limited PSF, the phase-ramped image
// against the analytic image at the shifted cell centres.
void SubCellShift()
{
   bool ok = true;
   double worst = 0;
   for (double frac : {0.37, 0.81}) {
      WidefieldSceneSpec s = BaseSpec(24, 1, frac);
      s.slabHalfUm = 0.3;
      SquareIllumination ill(3.0, 3.0);
      unsigned fx = 0, fy = 0;
      double frx = 0, fry = 0;
      const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s, &fx, &fy, &frx, &fry);
      TestPsf psf(g.pitchUm, 0.25, 0.0, 16);
      WidefieldDyeGrid G;
      G.spec = g;
      G.k0 = static_cast<long>(std::floor(s.focusWorldUm / g.zPlaneUm));
      G.nz = 1;
      G.persistent.assign((size_t)g.nx * g.ny, 0.0f);
      std::mt19937 rng(5);
      for (auto& v : G.persistent) { v = rng() % 7 == 0 ? 1.0f : 0.0f; G.nPersistent += (long)v; }
      WidefieldScene sc;
      std::string err;
      ok = ok && sc.UpdateFromGrid(G, ill, s, psf, err);
      std::vector<float> wb, cam;
      sc.FreshBleachWeights(0, wb);
      sc.RenderFrame(wb, cam);
      const std::vector<float> wp = Wp(sc, s);
      // Plane p: t = (centre - focus) / 0.1 = 0.5 * 0.25 ... PlaneCoord of the
      // plane centre; sigma does not depend on p (slope 0).
      const double sc_ = psf.SigmaCells(0), norm = psf.NormSum(0, sc.KernelRadius());
      std::vector<float> ref((size_t)s.width * s.height, 0.0f);
      for (unsigned Y = 0; Y < s.height; Y++)
         for (unsigned X = 0; X < s.width; X++) {
            const double cx = fx + X + frx, cy = fy + Y + fry; // the camera cell's grid position
            double acc = 0;
            for (unsigned iy = 0; iy < g.ny; iy++)
               for (unsigned ix = 0; ix < g.nx; ix++) {
                  const float a = G.persistent[(size_t)iy * g.nx + ix];
                  if (a == 0) continue;
                  const double dx = cx - ix, dy = cy - iy;
                  acc += a * wp[(size_t)iy * g.nx + ix] * std::exp(-(dx * dx + dy * dy) / (2 * sc_ * sc_)) / norm;
               }
            ref[(size_t)Y * s.width + X] = (float)acc;
         }
      const double d = MaxRelDiff(cam, ref);
      worst = std::max(worst, d);
      ok = ok && std::fabs(frx - frac) < 1e-6 && std::fabs(fry - frac) < 1e-6 && d < 1e-4;
   }
   char msg[160];
   std::snprintf(msg, sizeof msg, "sub-cell stage position: phase-ramped image == analytic shifted image (max rel diff %.2e)", worst);
   Check(ok, msg);
}

void PhotonConservation()
{
   WidefieldSceneSpec s = BaseSpec(64, 2);
   SquareIllumination ill(6.4, 6.4);
   const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s);
   GaussianWidefieldPsf psf(g.pitchUm, 660, 1.4, 1.518);
   WidefieldDyeGrid G;
   G.spec = g;
   G.k0 = static_cast<long>(std::floor(s.focusWorldUm / g.zPlaneUm));
   G.nz = 1;
   G.persistent.assign((size_t)g.nx * g.ny, 0.0f);
   for (unsigned iy = 40; iy < 88; iy++)
      for (unsigned ix = 40; ix < 88; ix++) { G.persistent[(size_t)iy * g.nx + ix] = 2; G.nPersistent += 2; }
   WidefieldScene sc;
   std::string err;
   sc.UpdateFromGrid(G, ill, s, psf, err);
   std::vector<float> wb, cam;
   sc.FreshBleachWeights(0, wb);
   sc.RenderFrame(wb, cam);
   const double dD = s.phot.EmissionRatePerSec(1.0) * s.exposureSec;
   const double expect = G.nPersistent * s.eta * dD, got = Sum(cam);
   char msg[160];
   std::snprintf(msg, sizeof msg, "photon conservation: %.1f vs dyes x eta x dD = %.1f", got, expect);
   Check(std::fabs(got / expect - 1) < 1e-4, msg);
}

void Units()
{
   WidefieldPhotophysics ph;
   const double eta = WidefieldCollectionEfficiency(1.4, 1.518);
   const double t = ph.HalfTimeSec(1.0), perFrame = eta * ph.EmissionRatePerSec(1.0) * 0.05;
   char msg[200];
   std::snprintf(msg, sizeof msg, "defaults: sigma %.4g um^2, eta %.4f, t1/2 %.3f s, %.3f photons/dye/frame",
                 ph.CrossSectionUm2(), eta, t, perFrame);
   Check(std::fabs(t - 120.0) < 0.2 && std::fabs(perFrame - 0.45) < 0.0125 && std::fabs(eta - 0.307) < 0.001, msg);
   WidefieldPhotophysics never = ph;
   never.photonBudget = 0;
   Check(std::isinf(never.HalfTimeSec(1.0)) && WidefieldBleachingPhotons(eta, 0, 1e6, 10) == eta * 10,
         "budget 0 = never bleaches");
}

void FrameIntegral()
{
   const double eta = 0.3, B = 5000, d0 = 1234, dD = 800;
   double num = 0;
   const int n = 200000;
   for (int i = 0; i < n; i++) num += eta * std::exp(-(d0 + (i + 0.5) * dD / n) / B) * dD / n;
   const double cf = WidefieldBleachingPhotons(eta, B, d0, dD);
   Check(std::fabs(cf / num - 1) < 1e-8, "bleaching frame integral == numerical integration");
}

void SplitRespected()
{
   WidefieldSceneSpec s = BaseSpec(32, 1);
   SquareIllumination ill(3.2, 3.2);
   const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s);
   TestPsf psf(g.pitchUm);
   std::string err;
   // Persistent only: constant frames.
   WidefieldScene sp;
   sp.UpdateFromGrid(RandomGrid(g, false, true, 3), ill, s, psf, err);
   std::vector<float> wb, a, b;
   sp.FreshBleachWeights(0, wb);
   sp.RenderFrame(wb, a);
   sp.FreshBleachWeights(5000, wb);
   sp.RenderFrame(wb, b);
   Check(Sum(a) > 0 && a == b, "persistent-only frames are constant");
   // Bleaching only: decays with t1/2.
   WidefieldScene sb;
   sb.UpdateFromGrid(RandomGrid(g, true, false, 4), ill, s, psf, err);
   const double fHalf = s.phot.HalfTimeSec(1.0) / s.exposureSec;
   sb.FreshBleachWeights(0, wb);
   a.clear();
   sb.RenderFrame(wb, a);
   sb.FreshBleachWeights(fHalf, wb);
   b.clear();
   sb.RenderFrame(wb, b);
   const double r = Sum(b) / Sum(a);
   char msg[120];
   std::snprintf(msg, sizeof msg, "bleaching-only signal at t1/2 = %.4f of frame 0 (fast path %d)", r, sb.LastFrameFast());
   Check(std::fabs(r - 0.5) < 1e-4 && sb.LastFrameFast(), msg);
}

void Illumination()
{
   SquareIllumination sq(2.0, 1.0);
   double x0, y0, x1, y1;
   sq.Support(x0, y0, x1, y1);
   std::vector<float> smp(8 * 4);
   sq.Sample(-1.2, -0.6, 0.3, 8, 4, smp.data());
   // Cell centres at -1.05 + 0.3 i: inside for i = 1..6; y centres -0.45 + 0.3 j: inside for j = 0..3.
   bool sOk = true;
   for (unsigned j = 0; j < 4; j++)
      for (unsigned i = 0; i < 8; i++) sOk = sOk && smp[j * 8 + i] == ((i >= 1 && i <= 6) ? 1.0f : 0.0f);
   Check(x0 == -1 && x1 == 1 && y0 == -0.5 && y1 == 0.5 && sq.At(0.99, 0.49) == 1 && sq.At(1.0, 0) == 0 && sOk,
         "SquareIllumination: support and sampling");
   BleachField bf;
   bf.Reset(0.05);
   bf.Deposit(sq, 10.0, -3.0, 7.0);
   std::vector<float> in, out;
   bf.DoseOver(9.2, -3.4, 30, 14, in);    // inside the square
   bf.DoseOver(11.2, -3.4, 30, 14, out);  // just beside it
   Check(std::all_of(in.begin(), in.end(), [](float v) { return v == 7.0f; }) &&
            std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }),
         "BleachField: deposit inside the pattern, 0 outside");
   std::vector<float> neg;
   bf.Reset(0.05);
   bf.Deposit(sq, -20.0, -30.0, 2.0);
   bf.DoseOver(-20.5, -30.2, 20, 8, neg);
   Check(std::all_of(neg.begin(), neg.end(), [](float v) { return v == 2.0f; }), "BleachField: negative tiles");
}

void FastEqualsFull()
{
   WidefieldSceneSpec s = BaseSpec(32, 2);
   SquareIllumination ill(3.2, 3.2);
   const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s);
   TestPsf psf(g.pitchUm);
   const WidefieldDyeGrid G = RandomGrid(g, true, true, 5);
   std::string err;
   WidefieldScene fast, full;
   fast.UpdateFromGrid(G, ill, s, psf, err);
   full.UpdateFromGrid(G, ill, s, psf, err);
   std::vector<float> wb, a, b, c, d;
   fast.FreshBleachWeights(0, wb);
   fast.RenderFrame(wb, a);
   fast.FreshBleachWeights(400, wb);
   fast.RenderFrame(wb, b);
   const bool wasFast = fast.LastFrameFast();
   full.FreshBleachWeights(400, wb);
   full.SetBleachWeights(wb);
   full.RenderCoefficients({1.0}, c);
   char msg[120];
   std::snprintf(msg, sizeof msg, "fast path == full path (max rel diff %.2e)", MaxRelDiff(b, c));
   Check(wasFast && MaxRelDiff(b, c) < 1e-5, msg);
   // Determinism: a fresh scene, the same bits.
   WidefieldScene again;
   again.UpdateFromGrid(G, ill, s, psf, err);
   again.FreshBleachWeights(0, wb);
   again.RenderFrame(wb, d);
   Check(a == d, "determinism: same inputs, same bits");
}

// A focus change re-pairs cached plane spectra: same bits as a fresh scene
// at that focus; and FocusSeries == one scene per focus.
void FocusReuse()
{
   WidefieldSceneSpec s = BaseSpec(40, 1);
   s.slabHalfUm = 3.0;
   SquareIllumination ill(4.0, 4.0);
   const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s);
   ScalarPsf psf(g.pitchUm, 24);
   const WidefieldDyeGrid G = RandomGrid(g, true, true, 31, s.focusWorldUm - 1.5, 120, 60);
   std::string err;
   WidefieldScene moving;
   moving.UpdateFromGrid(G, ill, s, psf, err);
   std::vector<float> wb, a, b;
   moving.FreshBleachWeights(10, wb);
   moving.RenderFrame(wb, a);
   const size_t bytes0 = moving.CachedSpectraBytes();
   WidefieldSceneSpec s2 = s;
   s2.focusWorldUm += 0.437;
   const auto t0 = std::chrono::steady_clock::now();
   moving.UpdateFromGrid(G, ill, s2, psf, err); // same grid: UpdateFromGrid rebuilds; use Refocus via a plain spec change below
   (void)t0;
   // Refocus through the normal path: a scene fed once, then focus moved.
   WidefieldScene sc;
   sc.UpdateFromGrid(G, ill, s, psf, err);
   sc.FreshBleachWeights(10, wb);
   sc.RenderFrame(wb, a);
   std::vector<double> foci = {s.focusWorldUm + 0.437, s.focusWorldUm - 0.9, s.focusWorldUm + 1.3};
   std::vector<WidefieldImages> series;
   const bool sOk = sc.FocusSeries(foci, series, err);
   bool same = sOk && series.size() == foci.size();
   double worst = 0;
   for (size_t i = 0; same && i < foci.size(); i++) {
      WidefieldSceneSpec si = s;
      si.focusWorldUm = foci[i];
      si.slabCentreUm = foci[i];
      WidefieldScene fresh;
      fresh.UpdateFromGrid(G, ill, si, psf, err);
      std::vector<float> ra, rb;
      fresh.FreshBleachWeights(10, wb);
      fresh.RenderFrame(wb, ra);
      std::vector<double> coef;
      sc.BleachCoefficients(wb, coef);
      series[i].Render(coef, rb);
      worst = std::max(worst, MaxRelDiff(rb, ra));
      same = same && ra == rb;
   }
   char msg[200];
   std::snprintf(msg, sizeof msg, "FocusSeries (3 foci, one spectra cache) == a fresh scene per focus (identical bits: %d, max rel %.1e, %.1f MB spectra)",
                 (int)same, worst, bytes0 / 1048576.0);
   Check(same, msg);
}

// Bleach basis with a non-uniform illumination: frames stay on the fast
// path and match a scene anchored at that very frame.
void BleachBasis()
{
   for (int kind = 0; kind < 2; kind++) {
      WidefieldSceneSpec s = BaseSpec(32, 1);
      std::unique_ptr<IlluminationPattern> ill;
      if (kind == 0) ill.reset(new TwoLevelIllumination(3.2, 3.2));
      else ill.reset(new BeamIllumination(3.2, 1.0));
      const WidefieldGridSpec g = WidefieldScene::GridSpecFor(*ill, s);
      TestPsf psf(g.pitchUm);
      const WidefieldDyeGrid G = RandomGrid(g, true, true, 41 + kind);
      std::string err;
      WidefieldScene sc;
      sc.UpdateFromGrid(G, *ill, s, psf, err);
      std::vector<float> wb, img;
      sc.FreshBleachWeights(0, wb);
      sc.RenderFrame(wb, img);
      bool allFast = true;
      double worst = 0;
      for (double f : {1.0, 50.0, 400.0, 900.0}) {
         sc.FreshBleachWeights(f, wb);
         std::vector<float> a, b;
         sc.RenderFrame(wb, a);
         allFast = allFast && sc.LastFrameFast();
         WidefieldScene ref;
         ref.UpdateFromGrid(G, *ill, s, psf, err);
         ref.SetBleachWeights(wb);
         std::vector<double> one;
         ref.BleachCoefficients(wb, one);
         ref.RenderCoefficients(one, b);
         worst = std::max(worst, MaxRelDiff(a, b));
      }
      char msg[200];
      std::snprintf(msg, sizeof msg, "bleach basis (%s illumination): frames 1..900 on the fast path %d, == re-anchored (max rel %.1e)",
                    kind == 0 ? "two-level, groups" : "Gaussian beam, Chebyshev", (int)allFast, worst);
      Check(allFast && worst < 1e-4, msg);
   }
}

// The GPU hosts' algorithm on the CPU (full precision): images from a
// WidefieldGpuJob alone, planes kept "resident" by key as the hosts do. The
// scene driven through it must match its own CPU images.
class JobReferenceHost : public WidefieldAccelerator
{
public:
   bool HasPlane(unsigned long long geometry, unsigned long long key) const override
   {
      return geometry == geometry_ && planes_.count(key) != 0;
   }
   bool Images(const WidefieldGpuJob& job, std::vector<std::vector<float>>& out, std::string& err) override
   {
      if (job.geometry != geometry_ || !fft_.Valid() || fft_.Nx() != job.NX || fft_.Ny() != job.NY)
      {
         planes_.clear();
         geometry_ = job.geometry;
         fft_ = RealFft2d(job.NX, job.NY);
      }
      for (const WidefieldGpuJob::Plane& p : job.planes)
      {
         std::vector<float> img(static_cast<size_t>(job.nx) * job.ny, 0.0f);
         for (size_t i = 0; i < p.cells.size(); i++) img[p.cells[i]] = p.values[i];
         std::vector<cfloat>& S = planes_[p.key];
         S.resize(fft_.SpecSize());
         fft_.Forward(img.data(), job.nx, job.ny, job.nx, S.data());
         jobPlanes_++;
      }
      std::map<int, const std::vector<cfloat>*> K;
      for (const auto& k : job.kernels) K[k.p] = k.spec;
      out.clear();
      const unsigned W = job.NX / 2 + 1;
      for (const auto& ch : job.channels)
      {
         std::vector<cfloat> acc(fft_.SpecSize(), cfloat(0, 0));
         for (const auto& d : ch)
         {
            auto it = planes_.find(d.key);
            if (it == planes_.end() || !K.count(d.p0) || (d.two && !K.count(d.p1)))
            {
               err = "job refers to a plane or kernel it does not carry";
               return false;
            }
            const std::vector<cfloat>& A = it->second;
            for (size_t b = 0; b < acc.size(); b++)
            {
               cfloat k = d.w0 * (*K[d.p0])[b];
               if (d.two) k += d.w1 * (*K[d.p1])[b];
               acc[b] += A[b] * k;
            }
         }
         for (unsigned ky = 0; ky < job.NY; ky++)
            for (unsigned kx = 0; kx < W; kx++)
            {
               const double kys = ky <= job.NY / 2 ? double(ky) : double(ky) - job.NY;
               const double ax = 2 * kPi * kx * job.fracX / job.NX, ay = 2 * kPi * kys * job.fracY / job.NY;
               const std::complex<double> px = kx == job.NX / 2 ? std::complex<double>(std::cos(ax), 0) : std::polar(1.0, ax);
               const std::complex<double> py = ky == job.NY / 2 ? std::complex<double>(std::cos(ay), 0) : std::polar(1.0, ay);
               const std::complex<double> ph = px * py;
               cfloat& v = acc[(size_t)ky * W + kx];
               v = cfloat(std::complex<double>(v) * ph);
            }
         std::vector<float> img((size_t)job.cw * job.ch);
         fft_.Inverse(acc.data(), img.data(), job.fovY0, job.ch, job.fovX0, job.cw, job.cw);
         out.push_back(std::move(img));
      }
      jobs_++;
      return true;
   }
   unsigned long long geometry_ = ~0ull;
   RealFft2d fft_;
   std::map<unsigned long long, std::vector<cfloat>> planes_;
   int jobs_ = 0;
   long jobPlanes_ = 0;
};

double ImagesRms(const WidefieldImages& a, const WidefieldImages& b)
{
   double e = 0, s = 0;
   auto acc = [&](const std::vector<float>& x, const std::vector<float>& y) {
      for (size_t i = 0; i < x.size() && i < y.size(); i++) { const double d = x[i] - y[i]; e += d * d; s += (double)y[i] * y[i]; }
      if (x.size() != y.size()) e += 1e30;
   };
   acc(a.persistent, b.persistent);
   if (a.bleach.size() != b.bleach.size()) return 1e30;
   for (size_t j = 0; j < a.bleach.size(); j++) acc(a.bleach[j], b.bleach[j]);
   return s > 0 ? std::sqrt(e / s) : std::sqrt(e);
}

// GPU mode through the job interface: a scene with the reference host vs a
// plain GPU-mode scene, over focus changes and a sub-cell move (planes stay
// resident: the later jobs carry none).
void GpuJobs()
{
   CellFieldSource src;
   CellFieldSettings cf;
   cf.seed = 42 ^ 0x43454C4Cu;
   cf.labels = { MakeLabelVector(ISC_MODE_DNA_PAINT, 0.6, 1, 0.01, 0.05, 1, 1, 0.5) };
   std::string err;
   if (!src.Configure(cf, err)) { Check(false, err.c_str()); return; }
   WidefieldSceneSpec s = BaseSpec(40, 1);
   s.originXUm = -2.0 + 0.0123;
   s.originYUm = -2.0 - 0.031;
   s.focusWorldUm = s.slabCentreUm = 1.0;
   s.slabHalfUm = 3.5;
   SquareIllumination ill(4.0, 4.0);
   GaussianWidefieldPsf psf(0.1, 660, 1.4, 1.518);
   JobReferenceHost host;
   WidefieldScene g, c;
   g.SetGpuMode(true);
   c.SetGpuMode(true);
   g.SetAccelerator(&host);
   double worst = 0;
   bool ok = true;
   long planesAfterFirst = -1;
   for (double f : {1.0, 1.3, 0.4, 0.4}) {
      s.focusWorldUm = s.slabCentreUm = f;
      if (f == 0.4 && planesAfterFirst >= 0) s.originXUm += 0.004; // the last one: a sub-cell move
      ok = ok && g.Update(src, ill, s, psf, err) && c.Update(src, ill, s, psf, err);
      std::vector<float> wb, a, b;
      g.FreshBleachWeights(5, wb);
      c.FreshBleachWeights(5, wb);
      g.RenderFrame(wb, a);
      c.RenderFrame(wb, b);
      worst = std::max(worst, ImagesRms(g.Images(), c.Images()));
      if (planesAfterFirst < 0) planesAfterFirst = host.jobPlanes_;
   }
   const bool pow2 = (g.FftSizeX() & (g.FftSizeX() - 1)) == 0 && (g.FftSizeY() & (g.FftSizeY() - 1)) == 0;
   char msg[240];
   std::snprintf(msg, sizeof msg, "GPU job interface (CPU reference host): %d jobs, %ld plane spectra (%ld after the first focus), %ux%u FFT, images rms %.1e vs the scene's own%s%s",
                 host.jobs_, host.jobPlanes_, host.jobPlanes_ - planesAfterFirst, g.FftSizeX(), g.FftSizeY(), worst,
                 g.UsingAccelerator() ? "" : ", accelerator dropped: ", g.GpuError().c_str());
   Check(ok && pow2 && g.UsingAccelerator() && worst < 1e-5 && host.jobs_ >= 4, msg);
}

void RealWorld()
{
   CellFieldSource src;
   CellFieldSettings cf;
   cf.seed = 42 ^ 0x43454C4Cu;
   cf.labels = { MakeLabelVector(ISC_MODE_PALM, 0.7, 1, 0.01, 0.05, 1, 1, 0.5) };
   std::string err;
   if (!src.Configure(cf, err)) { Check(false, err.c_str()); return; }
   WidefieldSceneSpec s = BaseSpec(48, 1);
   s.originXUm = 22 - 2.4;   // a cell of this seed covers (22, 14), in its lamella
   s.originYUm = 14 - 2.4;
   s.focusWorldUm = s.slabCentreUm = 0.5;
   s.slabHalfUm = 3.5;
   SquareIllumination ill(4.8, 4.8);
   GaussianWidefieldPsf psf(0.1, 660, 1.4, 1.518);
   WidefieldScene sc;
   const bool ok = sc.Update(src, ill, s, psf, err);
   std::vector<float> wb, cam;
   sc.FreshBleachWeights(0, wb);
   sc.RenderFrame(wb, cam);
   char msg[200];
   std::snprintf(msg, sizeof msg, "cell field: %ld dyes (%ld bleaching), %ux%u FFT, R %d, %d PSF planes, %.0f photons",
                 sc.Dyes(), sc.BleachingDyes(), sc.FftSizeX(), sc.FftSizeY(), sc.KernelRadius(), sc.PsfPlanes(), Sum(cam));
   Check(ok && sc.Dyes() > 1000 && sc.BleachingDyes() > 0 && Sum(cam) > 0, msg);

   // Tiles == one query over the same rect.
   WidefieldGridSpec g = sc.Grid();
   WidefieldDyeGrid one;
   const bool q = BuildWidefieldDyeGrid(src, g, one, err);
   WidefieldScene ref;
   ref.UpdateFromGrid(one, ill, s, psf, err);
   std::vector<float> cam2;
   ref.FreshBleachWeights(0, wb);
   ref.RenderFrame(wb, cam2);
   std::snprintf(msg, sizeof msg, "dye tiles == one density query: %ld vs %ld dyes, image max rel diff %.1e", sc.Dyes(),
                 one.nBleaching + one.nPersistent, MaxRelDiff(cam, cam2));
   Check(q && sc.Dyes() == one.nBleaching + one.nPersistent && MaxRelDiff(cam, cam2) < 1e-3, msg);

   // A sub-cell stage move keeps the dye binning (tiles and spectra reused).
   WidefieldSceneSpec s3 = s;
   s3.originXUm += 0.004;
   const size_t tileBytes = sc.Tiles()->Bytes();
   sc.Update(src, ill, s3, psf, err);
   Check(sc.Tiles()->Bytes() == tileBytes && sc.Grid().SameRect(g), "sub-cell stage move: same grid rect, no new tiles");
}

} // namespace

// Movies made one after the other in one process share the world and the
// WideField scene (ScopeMovie's MovieCache): a repeat, or a return after a
// movie in another world or with another PSF, must equal a fresh render.
void RepeatedMovies()
{
   auto movie = [](const char* text) {
      ScopeSpec spec;
      std::string err;
      std::vector<uint16_t> all;
      ScopeMovieInfo info;
      if (!ParseScopeSpec(text, spec, err) ||
          !RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& adu) {
             all.insert(all.end(), adu.begin(), adu.end());
             return true;
          }, info, err))
         std::printf("  movie failed: %s\n", err.c_str());
      return all;
   };
   const char* sr = "size=32 frames=4 x=30 y=10 psf-kernel-half-width-nm=1500 mt-imager-nm=20";
   const char* wf = "size=32 frames=3 x=30 y=10 mt-mode=WideField psf-kernel-half-width-nm=1500";
   const char* wfGauss = "size=32 frames=2 x=30 y=10 mt-mode=WideField psf-model=0 wf-upscale=2";
   const char* other = "size=32 frames=2 x=-20 y=5 world-seed=7 mt-mode=WideField psf-kernel-half-width-nm=1500";
   const std::vector<uint16_t> sr1 = movie(sr), wf1 = movie(wf), g1 = movie(wfGauss);
   const std::vector<uint16_t> sr2 = movie(sr), wf2 = movie(wf);
   Check(!sr1.empty() && sr1 == sr2, "SR movie repeated in one process = the first (shared world)");
   Check(!wf1.empty() && wf1 == wf2, "WideField movie repeated = the first (shared scene and PSF)");
   const std::vector<uint16_t> o = movie(other);
   const std::vector<uint16_t> wf3 = movie(wf), g2 = movie(wfGauss), sr3 = movie(sr);
   Check(!o.empty() && wf1 == wf3 && sr1 == sr3, "movies after another world and PSF = the first");
   Check(!g1.empty() && g1 == g2, "Gaussian WideField movie after kernel movies = the first");
}

// Issue 16: a continuous population's mean-field image (the structure's FFT
// image x the mean photons per dye) equals the per-dye path's (every dye's
// PSF through the running image) on average: 1 % in total, 4 % per bright
// pixel at 2 % labelling (as web/lab/check.mjs on the JS). Bright and with no
// read noise, offset or gain spread, so the ADU are the photon image.
void MeanFieldVsPerDye()
{
   auto movie = [](const std::string& text, std::vector<double>& mean) {
      ScopeSpec spec;
      std::string err;
      ScopeMovieInfo info;
      mean.clear();
      long frames = 0;
      if (!ParseScopeSpec(text, spec, err) ||
          !RenderScopeMovie(spec, [&](long, const std::vector<uint16_t>& adu) {
             mean.resize(adu.size(), 0.0);
             for (size_t i = 0; i < adu.size(); ++i) mean[i] += adu[i];
             frames++;
             return true;
          }, info, err))
         std::printf("  movie failed: %s\n", err.c_str());
      for (double& v : mean) v /= std::max(1L, frames);
   };
   const std::string base = "size=32 frames=4 x=30 y=10 mt-dye=mEGFP light-preset=auto mt-label-pct=2 start-sec=0 laser-488=20 "
                            "gain=50 offset=0 offset-std=0 read-noise=0 dark-per-sec=0 gain-std-pct=0 read-noise-std-pct=0 "
                            "psf-kernel-half-width-nm=1500 mt-dye.photon-budget=1e12 wf-upscale=3 ";
   std::vector<double> mf, pd;
   movie(base + "mean-field-density-per-um2=0 mean-field-max-emitters=0", mf);
   movie(base + "mean-field-density-per-um2=1e9 mean-field-max-emitters=1e9", pd);
   // The mean-field grid bins each dye to its cell and 25 nm plane, and blends
   // two PSF planes; the per-dye path splats the nearest plane at the exact
   // position: a few % per pixel near a sparse dye (rms), none in total.
   double sm = 0, sp = 0, peak = 0, worst = 0, e2 = 0;
   long nb = 0;
   for (size_t i = 0; i < mf.size() && i < pd.size(); ++i) { sm += mf[i]; sp += pd[i]; peak = std::max(peak, mf[i]); }
   for (size_t i = 0; i < mf.size() && i < pd.size(); ++i)
      if (mf[i] > 0.25 * peak)
      {
         const double r = pd[i] / mf[i] - 1;
         worst = std::max(worst, std::fabs(r));
         e2 += r * r;
         nb++;
      }
   const double rms = std::sqrt(e2 / std::max(1L, nb));
   char b[240];
   std::snprintf(b, sizeof b,
                 "mean-field = per-dye mean image: total %+.3f %%, bright pixels rms %.2f %% (worst %.1f %%, %ld pixels, mean %.0f ADU)",
                 100 * (sp / std::max(1.0, sm) - 1), 100 * rms, 100 * worst, nb, sm / std::max<size_t>(1, mf.size()));
   Check(!mf.empty() && std::fabs(sp / sm - 1) < 0.01 && rms < 0.04 && worst < 0.1, b);
}

int main()
{
   RepeatedMovies();
   MeanFieldVsPerDye();
   FftVsDft();
   ConvolutionVsDirect();
   FocusBands();
   SubCellShift();
   PhotonConservation();
   Units();
   FrameIntegral();
   SplitRespected();
   Illumination();
   FastEqualsFull();
   FocusReuse();
   BleachBasis();
   GpuJobs();
   RealWorld();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall widefield checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
