// Fiducials.cpp -- see Fiducials.h.
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

#include "Fiducials.h"
#include "DyeLibrary.h"
#include "Spectra.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sim {

namespace {

constexpr double kLn2 = 0.6931471805599453;   // JS Math.LN2
constexpr double kPiF = 3.141592653589793;

struct Tap
{
   int dx, dy;
   double w;
};

// The projected sphere of radius rS (kernel samples) as normalized taps, each tap's weight its kBeadLateralSub^2
// sub-samples of sqrt(r^2 - rho^2) (JS sphereTaps).
std::vector<Tap> SphereTaps(double rS)
{
   const int R = static_cast<int>(std::ceil(rS + 0.5)), n = kBeadLateralSub;
   std::vector<Tap> taps;
   double sum = 0.0;
   for (int dy = -R; dy <= R; dy++)
      for (int dx = -R; dx <= R; dx++)
      {
         double w = 0.0;
         for (int sy = 0; sy < n; sy++)
            for (int sx = 0; sx < n; sx++)
            {
               const double u = dx + (sx + 0.5) / n - 0.5, v = dy + (sy + 0.5) / n - 0.5, q = rS * rS - (u * u + v * v);
               if (q > 0.0)
                  w += std::sqrt(q);
            }
         if (w > 0.0)
         {
            taps.push_back({ dx, dy, w });
            sum += w;
         }
      }
   if (!(sum > 0.0))
      return { { 0, 0, 1.0 } };
   for (Tap& t : taps)
      t.w /= sum;
   return taps;
}

// img += photons x the block sums B (of geometry cache) at setup st over the camera pixels (x0 + dx, y0 + dy), dx in
// [dxLo, dxHi], dy in [dyLo, dyHi], clipped to the image and the block sums (JS splatRect; SplatRows' arithmetic).
void SplatRect(std::vector<float>& img, unsigned W, unsigned H, const PsfKernelCache& cache, const SplatSetupResult& st,
               const float* B, double photons, int dxLo, int dxHi, int dyLo, int dyHi)
{
   const int os = std::max(1, cache.oversampling), off = os - 1, bw = cache.blockSumWidth, NT = st.nTaps;
   const int yLo = std::max(dyLo, -st.y0), yHi = std::min(dyHi, static_cast<int>(H) - 1 - st.y0);
   const int xLo = std::max(dxLo, -st.x0), xHi = std::min(dxHi, static_cast<int>(W) - 1 - st.x0);
   for (int dy = yLo; dy <= yHi; ++dy)
   {
      const int r0 = st.by + dy * os + off, jLo = std::max(0, -r0), jHi = std::min(NT, bw - r0);
      float* rowOut = img.data() + static_cast<size_t>(st.y0 + dy) * W;
      for (int dx = xLo; dx <= xHi; ++dx)
      {
         const int c0 = st.bx + dx * os + off, iLo = std::max(0, -c0), iHi = std::min(NT, bw - c0);
         double sum = 0.0;
         for (int j = jLo; j < jHi; ++j)
         {
            const float* brow = B + static_cast<size_t>(r0 + j) * bw + c0;
            double row = 0.0;
            for (int i = iLo; i < iHi; ++i)
               row += st.wx[i] * brow[i];
            sum += st.wy[j] * row;
         }
         rowOut[st.x0 + dx] += static_cast<float>(photons * sum);
      }
   }
}

// A photon-conserving 2D Gaussian of any sign (JS renderGaussian; RenderGaussianPSF takes only positive photons).
void GaussianSigned(std::vector<float>& img, unsigned W, unsigned H, double xPx, double yPx, double sigmaPx,
                    double photons)
{
   if (photons == 0.0 || !(sigmaPx > 0.0))
      return;
   const int rad = static_cast<int>(std::ceil(3.0 * sigmaPx));
   const int cx = static_cast<int>(std::lround(xPx)), cy = static_cast<int>(std::lround(yPx));
   const double amplitude = photons / (2.0 * kPiF * sigmaPx * sigmaPx), twoSigmaSq = 2.0 * sigmaPx * sigmaPx;
   const int yLo = std::max(0, cy - rad), yHi = std::min(static_cast<int>(H) - 1, cy + rad);
   const int xLo = std::max(0, cx - rad), xHi = std::min(static_cast<int>(W) - 1, cx + rad);
   for (int py = yLo; py <= yHi; ++py)
   {
      const double ddy = py - yPx;
      float* row = img.data() + static_cast<size_t>(py) * W;
      for (int px = xLo; px <= xHi; ++px)
      {
         const double ddx = px - xPx;
         row[px] += static_cast<float>(amplitude * std::exp(-(ddx * ddx + ddy * ddy) / twoSigmaSq));
      }
   }
}

double MeanPhotonsOf(double ratePerSec, double lambda, double t0, double t1)
{
   return lambda > 0 ? ratePerSec * (std::exp(-lambda * t0) - std::exp(-lambda * t1)) / lambda
                     : ratePerSec * (t1 - t0);
}

} // namespace

double BeadBinNm(double dNm)
{
   return std::max(kBeadBinNm, kBeadBinNm * std::floor(dNm / kBeadBinNm + 0.5));
}

BeadKernel::BeadKernel(const PsfKernelCache& K, double pixelNm) : K_(K), pixelNm_(pixelNm)
{
   const int os = std::max(1, K.oversampling), camRad = K.halfWidthOversampled / os;
   rc_ = std::min(camRad, static_cast<int>(std::ceil(kBeadCoreUm * 1000.0 / pixelNm)));
   h_ = std::min(K.halfWidthOversampled, (rc_ + 2) * os);
   interp_ = K.interpMode == PsfInterpMode::Fft ? PsfInterpMode::Cubic : K.interpMode;
}

// The bead kernel's plane: a bead of diameter dNm at plane k, (2 h + 1)^2 samples (JS beadPlane).
std::vector<float> BeadKernel::Plane(int k, double dNm) const
{
   const PsfKernelCache& K = K_;
   const int n = K.sizeOversampled, c = K.halfWidthOversampled, os = std::max(1, K.oversampling), h = h_;
   const std::vector<std::vector<float>>& planes = K.Planes();
   const double sampleNm = pixelNm_ / os, rNm = dNm / 2.0;
   const std::vector<Tap> taps = SphereTaps(rNm / sampleNm);
   int R = 0;
   for (const Tap& t : taps)
      R = std::max(R, std::max(std::abs(t.dx), std::abs(t.dy)));
   // z average: per plane j its weight (the slabs' disc areas x their interpolation weights).
   const int nS = std::max(1, static_cast<int>(std::ceil(dNm / kBeadSlabNm)));
   std::vector<double> wz(static_cast<size_t>(K.nz), 0.0);
   double wsum = 0.0;
   for (int j = 0; j < nS; j++)
   {
      const double zr = -rNm + (j + 0.5) * dNm / nS, w = std::max(0.0, rNm * rNm - zr * zr);
      wsum += w;
      if (K.nz < 2)
      {
         wz[0] += w;
         continue;
      }
      const double u = std::min(static_cast<double>(K.nz - 1), std::max(0.0, k + zr / K.zStepNm));
      const int i0 = std::min(K.nz - 2, static_cast<int>(std::floor(u)));
      const double f = u - i0;
      wz[static_cast<size_t>(i0)] += w * (1.0 - f);
      wz[static_cast<size_t>(i0) + 1] += w * f;
   }
   const int Hh = h + R, m = 2 * Hh + 1;
   std::vector<double> avg(static_cast<size_t>(m) * m, 0.0);
   for (int j = 0; j < K.nz; j++)
   {
      if (!(wz[static_cast<size_t>(j)] > 0))
         continue;
      const float* pl = planes[static_cast<size_t>(j)].data();
      const double w = wz[static_cast<size_t>(j)] / wsum;
      for (int y = -Hh; y <= Hh; y++)
      {
         const int ky = c + y;
         if (ky < 0 || ky >= n)
            continue;
         for (int x = -Hh; x <= Hh; x++)
         {
            const int kx = c + x;
            if (kx >= 0 && kx < n)
               avg[static_cast<size_t>(y + Hh) * m + x + Hh] += w * pl[static_cast<size_t>(ky) * n + kx];
         }
      }
   }
   const int size = 2 * h + 1;
   std::vector<float> out(static_cast<size_t>(size) * size);
   const float* pk = planes[static_cast<size_t>(k)].data();
   const double fadeIn = (kBeadCoreUm - kBeadFadeUm) * 1000.0 / sampleNm, fadeLen = kBeadFadeUm * 1000.0 / sampleNm;
   for (int y = -h; y <= h; y++)
      for (int x = -h; x <= h; x++)
      {
         const int ky = c + y, kx = c + x;
         const double p = ky >= 0 && ky < n && kx >= 0 && kx < n ? pk[static_cast<size_t>(ky) * n + kx] : 0.0;
         const double rho = std::sqrt(static_cast<double>(x * x + y * y));
         const double fade = rho <= fadeIn ? 1.0 : std::max(0.0, 1.0 - (rho - fadeIn) / fadeLen);
         double b = p;
         if (fade > 0.0)
         {
            double s = 0.0;
            for (const Tap& t : taps)
               s += t.w * avg[static_cast<size_t>(y + t.dy + Hh) * m + x + t.dx + Hh];
            b = p + fade * (s - p);
         }
         out[static_cast<size_t>(y + h) * size + x + h] = static_cast<float>(b);
      }
   // The sphere's light that the z average spreads beyond the core: given back in the core, in proportion to the point
   // PSF there (the bead keeps the point's photons).
   double sp = 0.0, sb = 0.0;
   for (int y = -h; y <= h; y++)
      for (int x = -h; x <= h; x++)
      {
         const int ky = c + y, kx = c + x;
         if (ky >= 0 && ky < n && kx >= 0 && kx < n)
            sp += pk[static_cast<size_t>(ky) * n + kx];
         sb += out[static_cast<size_t>(y + h) * size + x + h];
      }
   if (sp > 0.0)
   {
      const double g = (sp - sb) / sp;
      for (int y = -h; y <= h; y++)
         for (int x = -h; x <= h; x++)
         {
            const int ky = c + y, kx = c + x;
            if (ky >= 0 && ky < n && kx >= 0 && kx < n)
            {
               float& o = out[static_cast<size_t>(y + h) * size + x + h];
               o = static_cast<float>(o + g * pk[static_cast<size_t>(ky) * n + kx]);
            }
         }
   }
   return out;
}

const BeadKernel::Core& BeadKernel::CoreAt(int k, double dBin)
{
   const long long key = static_cast<long long>(k) * 100000 + static_cast<long long>(dBin);
   std::lock_guard<std::mutex> lock(mu_);
   auto it = cores_.find(key);
   if (it != cores_.end())
      return *it->second;
   const int os = std::max(1, K_.oversampling), size = 2 * h_ + 1;
   std::unique_ptr<Core> C(new Core());
   const std::vector<float> plane = Plane(k, dBin);
   C->geom.oversampling = os;
   C->geom.halfWidthOversampled = h_;
   C->geom.sizeOversampled = size;
   C->geom.blockSumWidth = size + os - 1;
   C->blockSums = BuildBlockSums(plane.data(), size, os);
   return *cores_.emplace(key, std::move(C)).first->second;
}

void BeadKernel::Splat(std::vector<float>& img, unsigned W, unsigned H, int k, double dNm, double xPx, double yPx,
                       double photons)
{
   const PsfKernelCache& K = K_;
   if (photons == 0.0 || !K.valid || k < 0 || k >= K.nz)
      return;
   const Core& C = CoreAt(k, BeadBinNm(dNm));
   const int rc = rc_, camRad = K.halfWidthOversampled / std::max(1, K.oversampling);
   SplatRect(img, W, H, C.geom, SplatSetup(C.geom, xPx, yPx, interp_), C.blockSums.data(), photons, -rc, rc, -rc, rc);
   const SplatSetupResult st = SplatSetup(K, xPx, yPx, PsfInterpMode::Nearest);
   const float* P = K.BlockSums()[static_cast<size_t>(k)].data();
   SplatRect(img, W, H, K, st, P, photons, -camRad, camRad, -camRad, -rc - 1);
   SplatRect(img, W, H, K, st, P, photons, -camRad, camRad, rc + 1, camRad);
   SplatRect(img, W, H, K, st, P, photons, -camRad, -rc - 1, -rc, rc);
   SplatRect(img, W, H, K, st, P, photons, rc + 1, camRad, -rc, rc);
}

std::shared_ptr<BeadKernel> BeadKernelFor(const PsfKernelCache& K, double pixelNm)
{
   // A few recent (kernel, interpolation, pixel) entries: a movie's components and the next movie's.
   struct Entry
   {
      uint64_t serial;
      PsfInterpMode interp;
      double pixelNm;
      std::shared_ptr<BeadKernel> B;
   };
   static std::mutex mu;
   static std::vector<Entry> memo;
   std::lock_guard<std::mutex> lock(mu);
   for (const Entry& e : memo)
      if (e.serial == K.Serial() && e.interp == K.interpMode && e.pixelNm == pixelNm)
         return e.B;
   if (memo.size() >= 8)
      memo.erase(memo.begin());
   memo.push_back({ K.Serial(), K.interpMode, pixelNm, std::make_shared<BeadKernel>(K, pixelNm) });
   return memo.back().B;
}

double GoldScatteringUm2(double lambdaNm, double dNm)
{
   const GoldMieData& M = GoldMie();
   auto at = [](double minNm, double maxNm, double stepNm, double v, int& i, double& f) {
      const int n = static_cast<int>(std::floor((maxNm - minNm) / stepNm + 0.5));
      const double x = std::min(static_cast<double>(n), std::max(0.0, (v - minNm) / stepNm));
      i = std::min(n - 1, static_cast<int>(std::floor(x)));
      f = x - i;
      return n;
   };
   int i, j;
   double fi, fj;
   at(M.diameterMinNm, M.diameterMaxNm, M.diameterStepNm, dNm, i, fi);
   const int nL = at(M.lambdaMinNm, M.lambdaMaxNm, M.lambdaStepNm, lambdaNm, j, fj) + 1;
   auto S = [&](int a, int b) { return M.scatteringUm2[static_cast<size_t>(a) * nL + b]; };
   const double v = (S(i, j) * (1 - fj) + S(i, j + 1) * fj) * (1 - fi) + (S(i + 1, j) * (1 - fj) + S(i + 1, j + 1) * fj) * fi;
   // Below the table's smallest sphere: the dipole limit, sigma ~ d^6.
   return dNm < M.diameterMinNm ? v * std::pow(dNm / M.diameterMinNm, 6.0) : v;
}

bool FiducialPhotophysics(int t, const LightPath& lp, const FiducialLightOptions& o, std::vector<FiducialComponent>& out,
                          std::string& err)
{
   out.clear();
   if (t < 0 || t >= static_cast<int>(FiducialTypeIds().size()))
   {
      err = "fid-type " + std::to_string(t) + ": not one of 0.." + std::to_string(FiducialTypeIds().size() - 1);
      return false;
   }
   const FiducialTypeData& T = FiducialTypeAt(t);
   const double eqExt = FiducialEquivalentExtCoeff(), eqQy = FiducialEquivalentQy();
   for (int ci = 0; ci < T.componentCount; ++ci)
   {
      const FiducialComponentData& c = FiducialComponentAt(T.firstComponent + ci);
      const Spectrum* ex = SpectrumByKey(c.ex);
      const Spectrum* em = SpectrumByKey(c.em);
      if (!ex || !em)
      {
         err = std::string("fiducial component ") + c.id + ": no spectra";
         return false;
      }
      double emMaxNm = 0, best = -1;
      for (int i = 0; i < kSpectraGridN; i++)
         if ((*em)[static_cast<size_t>(i)] > best)
         {
            best = (*em)[static_cast<size_t>(i)];
            emMaxNm = SpectraGridNm(i);
         }
      const StateData sd = { true, 0.0, emMaxNm, eqExt, eqQy, false, c.ex, c.em, nullptr, false, 0.0, 0.0, 0.0, 0.0 };
      StatePhysics st;
      if (!StatePhotophysics(sd, lp, st, err))
         return false;
      const double kEmRef = eqQy * CrossSectionUm2(eqExt, *ex, c.exPeakNm) * PhotonFluxPerUm2(o.refKWcm2, c.exPeakNm);
      FiducialComponent fc;
      fc.id = c.id;
      fc.name = c.name;
      fc.lambdaNm = st.lambdaNm;
      fc.detectedPerSec = st.detectedPerSec;
      fc.detectedFraction = st.detectedFraction;
      for (const StatePhysics::LineShare& l : st.lineShares)
         fc.lineShares.push_back({ l.nm, l.share });
      fc.bleachPerSec = c.bleaches && o.halfSec > 0 && kEmRef > 0 ? st.emissionPerSec / kEmRef * kLn2 / o.halfSec : 0.0;
      fc.emitters = o.dyes > 0 ? o.dyes : c.emitters;
      fc.refDiameterNm = T.refDiameterNm;
      out.push_back(fc);
   }
   if (T.scatterGold)
   {
      const double floorT = std::pow(10.0, -std::max(0.0, o.blockingOD));
      for (const LaserLine& l : lp.lasers)
      {
         const double flux = PhotonFluxPerUm2(l.kWPerCm2 * (1 - SampleAt(lp.dichroicT, l.nm)), l.nm);
         const double tr = std::max(SampleAt(lp.dichroicT, l.nm) * SampleAt(lp.emissionT, l.nm), floorT) * SampleAt(lp.qe, l.nm);
         if (!(flux > 0))
            continue;
         char b[64];
         FiducialComponent fc;
         std::snprintf(b, sizeof b, "scatter%g", l.nm);
         fc.id = b;
         std::snprintf(b, sizeof b, "scattered %g nm", l.nm);
         fc.name = b;
         fc.scatter = true;
         fc.lambdaNm = l.nm;
         fc.perUm2 = flux * lp.eta * tr;
         fc.lineShares.push_back({ l.nm, 1.0 });
         out.push_back(fc);
      }
   }
   return true;
}

double BeadRatePerSec(const FiducialComponent& c, double dNm)
{
   return c.scatter ? c.perUm2 * GoldScatteringUm2(c.lambdaNm, dNm)
                    : c.emitters * std::pow(dNm / c.refDiameterNm, 3.0) * c.detectedPerSec;
}

namespace {

// One bead into img with `photons` at the focus zf and drift (dx, dy) px (JS splatFiducial).
void SplatFiducial(std::vector<float>& img, unsigned W, unsigned H, const FiducialMovieComponent& F,
                   const FiducialBeadSite& e, double zf, double pixelNm, double photons, double dx, double dy)
{
   const double xPx = e.xUm * 1000.0 / pixelNm + dx, yPx = e.yUm * 1000.0 / pixelNm + dy;
   if (!F.B)
   {
      // The Gaussian PSF: the sphere's projection adds r^2 / 5 to the variance per axis.
      const double rPx = e.d / 2.0 / pixelNm;
      GaussianSigned(img, W, H, xPx, yPx, std::sqrt(F.sigmaPx * F.sigmaPx + rPx * rPx / 5.0), photons);
      return;
   }
   F.B->Splat(img, W, H, F.kernel.NearestZIndex(e.zNm / 1000.0 - zf), e.d, xPx, yPx, photons);
}

} // namespace

void RenderFiducials(std::vector<float>& img, unsigned W, unsigned H, FiducialMovie& FM, long f, double zf,
                     double pixelNm, bool staticFocus, double dx, double dy)
{
   const double exp = FM.expSec, tm = (FM.N - 1) * exp / 2;
   for (FiducialMovieComponent& F : FM.comps)
   {
      if (staticFocus && F.taylorState < 0)
      {
         double lMax = 0.0;
         for (const FiducialBeadSite& e : F.beads)
            lMax = std::max(lMax, e.lambda);
         const double x = lMax * tm;
         int D = 0;
         double rem = x;   // x^(D+1) / (D+1)!
         while (rem > kFiducialTaylorTol && D < kFiducialTaylorMaxOrder)
         {
            D++;
            rem *= x / (D + 1);
         }
         F.taylorState = rem > kFiducialTaylorTol ? 0 : 1;
         F.taylorZ = zf;
         for (int k = 0; F.taylorState == 1 && k <= D; k++)
         {
            std::vector<float> image(static_cast<size_t>(W) * H, 0.0f);
            for (const FiducialBeadSite& e : F.beads)
            {
               // rate x e^-L (t0 + tm) x the frame integral's factor (1 - e^-L exp) / L, x (-L)^k.
               const double q = e.lambda > 0 ? (1.0 - std::exp(-e.lambda * exp)) / e.lambda : exp;
               const double w = e.rate * std::exp(-e.lambda * (e.t0Sec + tm)) * q * std::pow(-e.lambda, static_cast<double>(k));
               if (w != 0.0)
                  SplatFiducial(image, W, H, F, e, zf, pixelNm, w, 0.0, 0.0);
            }
            F.taylor.push_back(std::move(image));
         }
      }
      if (staticFocus && F.taylorState == 1 && zf == F.taylorZ)
      {
         const double u = f * exp - tm;
         double c = 1.0;
         for (size_t k = 0; k < F.taylor.size(); k++)
         {
            if (k > 0)
               c = c * u / static_cast<double>(k);
            const float mc = static_cast<float>(c);
            const std::vector<float>& image = F.taylor[k];
            for (size_t i = 0; i < img.size(); i++)
               if (image[i] != 0)
                  img[i] += static_cast<float>(static_cast<double>(mc) * image[i]);
         }
         continue;
      }
      for (const FiducialBeadSite& e : F.beads)
      {
         const double tf = e.t0Sec + f * exp;
         SplatFiducial(img, W, H, F, e, zf, pixelNm, MeanPhotonsOf(e.rate, e.lambda, tf, tf + exp), dx, dy);
      }
   }
}

} // namespace sim
