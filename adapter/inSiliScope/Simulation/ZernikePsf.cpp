///////////////////////////////////////////////////////////////////////////////
// FILE:          ZernikePsf.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See ZernikePsf.h. Every expression keeps the operand order
//                of the JS/Java originals; the only restructuring is that the
//                chirp-Z transform's input-independent parts (pre-chirp,
//                the filter's FFT, post-chirps), which depend only on the
//                grid and not on the line or the z plane, are computed once
//                per stack (CztPlan) instead of once per line.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "ZernikePsf.h"

#include "ChirpZ.h"
#include "FftRadix2.h"
#include "Parallel.h"
#include "SMLMZernike.h"
#include "Timing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace sim {

namespace {

constexpr int kFftM = 64; // webSMLM's PSF_FFT_M, Java's FFT_M
constexpr double kPi = 3.14159265358979323846;

// OSA/ANSI single index j -> (n, m): j = n(n+1)/2 + l, m = -n + 2l.
void IndexToNM(int j, int& n, int& m)
{
   for (int nn = 0;; ++nn)
      for (int l = 0; l <= nn; ++l)
         if (nn * (nn + 1) / 2 + l == j)
         {
            n = nn;
            m = -nn + 2 * l;
            return;
         }
}

double Binomial(int a, int b)
{
   if (b < 0 || b > a)
      return 0.0;
   double r = 1.0;
   for (int i = 0; i < b; ++i)
      r = r * (a - i) / (i + 1);
   return r;
}

// Unnormalized radial polynomial R_n^m (m >= 0).
double ZernikeRadial(int n, int m, double rho)
{
   double r = 0.0;
   for (int k = 0; k <= (n - m) / 2; ++k)
   {
      const double coeff = (k % 2 == 0 ? 1.0 : -1.0) * Binomial(n - k, k) * Binomial(n - 2 * k, (n - m) / 2 - k);
      r += coeff * std::pow(rho, static_cast<double>(n - 2 * k));
   }
   return r;
}

struct ZernikeMode
{
   int n = 0, l = 0, m = 0;
};

double ZernikeValue(const ZernikeMode& z, double rho, double phi)
{
   const double radial = rho <= 1.0 ? ZernikeRadial(z.n, z.m, rho) : 0.0;
   return radial * (z.l >= 0 ? std::cos(z.m * phi) : std::sin(z.m * phi));
}

double LaguerreL(int p, int a, double x)
{
   double lm1 = 0.0, l = 1.0; // L_0^a = 1
   for (int k = 0; k < p; ++k)
   {
      const double next = ((2 * k + 1 + a - x) * l - (k + a) * lm1) / (k + 1);
      lm1 = l;
      l = next;
   }
   return l;
}

// Java's pupilMaskPhase (Math.max(2, maskModes), no rounding).
double MaskPhase(bool doubleHelix, int maskModes, double maskWaist, double rhoNorm, double phi)
{
   if (!doubleHelix)
      return 0.0;
   const int n = std::max(2, maskModes);
   const double w = std::max(0.2, maskWaist);
   const double u = std::max(0.0, std::min(1.0, rhoNorm)) / w;
   const double u2 = u * u;
   double re = 0.0, im = 0.0;
   for (int p = 0; p < n; ++p)
   {
      const int l = 2 * p + 1;
      const double amp = std::pow(u, static_cast<double>(l)) * std::exp(-u2) * LaguerreL(p, l, 2.0 * u2);
      re += amp * std::cos(l * phi);
      im += amp * std::sin(l * phi);
   }
   return (re == 0.0 && im == 0.0) ? 0.0 : std::atan2(im, re);
}

struct StackGeometry
{
   int size = 0, nz = 0, oversampling = 1, halfOv = 0;
   double resLateralNm = 0.0;
};

StackGeometry GeometryFor(const PsfGeneratorRequest& req)
{
   // Same derivation as ComputePsfKernelCacheUncached's JVM call.
   StackGeometry g;
   g.oversampling = std::max(1, req.oversampling);
   const int camHalf = std::max(1, req.kernelHalfWidthPx);
   g.halfOv = camHalf * g.oversampling;
   g.size = 2 * g.halfOv + 1;
   const int nzWanted = std::max(3, req.nz);
   g.nz = (nzWanted % 2 == 1) ? nzWanted : nzWanted + 1;
   g.resLateralNm = req.pixelSizeNm / g.oversampling;
   return g;
}

} // namespace

bool ComputeZernikePsfPlanes(const PsfGeneratorRequest& req, std::vector<std::vector<float>>& out,
                             std::string& outError)
{
   out.clear();
   outError.clear();
   const StackGeometry g = GeometryFor(req);
   if (!(req.na > 0.0) || !(req.wavelengthNm > 0.0) || !(req.immersionIndex > 0.0) || !(req.sampleIndex > 0.0) ||
       !(req.pixelSizeNm > 0.0) || !(req.zStepNm > 0.0))
   {
      outError = "GibsonLanniZernike PSF: NA, wavelength, refractive indices, pixel size and z step must be > 0.";
      return false;
   }

   // PsfBridge.java's parameter mapping: SI units throughout.
   const double NA = req.na;
   const double lambda = req.wavelengthNm * 1E-9;
   const double ni = req.immersionIndex;
   const double ns = req.sampleIndex;
   const double ti0 = req.workingDistanceUm * 1E-6;
   const double particleAxialPosition = req.sampleDepthNm * 1E-9;
   const double resAxialM = req.zStepNm * 1E-9;
   const double resLateralM = g.resLateralNm * 1E-9;
   const bool doubleHelix = req.maskType == PsfMaskType::DoubleHelix;

   // PsfBridge.parseZernikeCoefficients: 15 or 28 values, else all zero.
   bool zOk = false;
   const ZernikeCoefficients coeffs = ParseZernikeCoefficients(req.zernikeCoefficients, zOk);
   std::vector<std::pair<double, ZernikeMode>> modes; // the nonzero terms, in index order
   for (size_t j = 0; j < coeffs.size(); ++j)
   {
      if (coeffs[j] == 0.0)
         continue;
      ZernikeMode zm;
      IndexToNM(static_cast<int>(j), zm.n, zm.l);
      zm.m = std::abs(zm.l);
      modes.emplace_back(coeffs[j], zm);
   }

   const double k0 = 2.0 * kPi / lambda;
   const double bMax = std::min(1.0, ns / NA);
   const double kMax = k0 * NA * bMax;
   const double dk = (2.0 * kMax) / (kFftM - 4);
   const double kMin = -std::floor(kFftM / 2.0) * dk;
   const int nx = g.size, ny = g.size;
   const double x0m = -((nx - 1) / 2.0) * resLateralM;
   // nx == ny, so x0 == y0 and both passes share one plan.
   const CztPlan plan(kFftM, dk, kMin, nx, resLateralM, x0m);

   // The z-independent part of the pupil: aperture, the sample-depth OPD,
   // the immersion-OPD obliquity factor, the Zernike and mask phases. Per
   // plane, phase = k0*(opd1+opd3) + zernikePhase + maskPhase as in the JS.
   const int M = kFftM, c0 = kFftM / 2;
   const size_t MM = static_cast<size_t>(M) * M;
   std::vector<char> inside(MM, 0);
   std::vector<double> opd1Tab(MM, 0.0), opd3Factor(MM, 0.0), zernTab(MM, 0.0), maskTab(MM, 0.0);
   for (int iy = 0; iy < M; ++iy)
   {
      const double ky = (iy - c0) * dk;
      for (int ix = 0; ix < M; ++ix)
      {
         const double kx = (ix - c0) * dk;
         const double kr2 = kx * kx + ky * ky;
         if (kr2 > kMax * kMax)
            continue; // zero outside the aperture (rho <= 1)
         const size_t idx = static_cast<size_t>(iy) * M + ix;
         inside[idx] = 1;
         const double kr = std::sqrt(kr2);
         const double rho = kr / (k0 * NA);
         const double phi = std::atan2(ky, kx);
         const double s1 = NA * rho / ns;
         const double s3 = NA * rho / ni;
         opd1Tab[idx] = ns * particleAxialPosition * std::sqrt(std::max(0.0, 1.0 - s1 * s1));
         opd3Factor[idx] = std::sqrt(std::max(0.0, 1.0 - s3 * s3));
         double zernikePhase = 0.0;
         for (const auto& t : modes)
            zernikePhase += t.first * ZernikeValue(t.second, rho / bMax, phi);
         zernikePhase *= 2.0 * kPi;
         zernTab[idx] = zernikePhase;
         maskTab[idx] = MaskPhase(doubleHelix, req.maskModes, req.maskWaist, rho / bMax, phi);
      }
   }

   const int nz = g.nz;
   out.assign(static_cast<size_t>(nz), std::vector<float>());
   const double focalShift = particleAxialPosition * (ni / ns);
   ParallelFor(static_cast<unsigned>(nz), [&](unsigned zi) {
      const int z = static_cast<int>(zi);
      const double ti = (ti0 - focalShift) + resAxialM * (z - (nz - 1.0) / 2.0);
      std::vector<double> pupilRe(MM, 0.0), pupilIm(pupilRe.size(), 0.0);
      for (size_t idx = 0; idx < pupilRe.size(); ++idx)
      {
         if (!inside[idx])
            continue;
         const double opd3 = ni * (ti - ti0) * opd3Factor[idx];
         const double phase = k0 * (opd1Tab[idx] + opd3) + zernTab[idx] + maskTab[idx];
         pupilRe[idx] = std::cos(phase);
         pupilIm[idx] = std::sin(phase);
      }

      // The transforms run kBatch lines at a time (CztPlan::Apply<kBatch>:
      // per line the one-line operations, so the same bits); a short last
      // batch is padded with zero lines whose outputs are dropped.
      constexpr int kBatch = 4;
      std::vector<double> aRe(plan.ScratchPerBatch(kBatch)), aIm(aRe.size());
      const std::vector<double> zeroLine(static_cast<size_t>(M), 0.0);
      std::vector<double> lineRe(static_cast<size_t>(M) * kBatch), lineIm(lineRe.size());
      const int oLen = std::max(nx, ny);
      std::vector<double> oRe(static_cast<size_t>(oLen) * kBatch), oIm(oRe.size());
      const double* inR[kBatch];
      const double* inI[kBatch];
      double* outR[kBatch];
      double* outI[kBatch];
      for (int b = 0; b < kBatch; ++b)
      {
         outR[b] = oRe.data() + static_cast<size_t>(b) * oLen;
         outI[b] = oIm.data() + static_cast<size_t>(b) * oLen;
      }
      // Row pass: one CZT over ky per pupil column m -> M x ny intermediate
      // (all-zero columns transform to exactly zero and are skipped).
      std::vector<double> midRe(static_cast<size_t>(M) * ny, 0.0), midIm(midRe.size(), 0.0);
      std::vector<int> cols;
      for (int m = 0; m < M; ++m)
      {
         bool any = false;
         for (int n = 0; n < M && !any; ++n)
            any = inside[static_cast<size_t>(n) * M + m] != 0;
         if (any)
            cols.push_back(m);
      }
      for (size_t c0 = 0; c0 < cols.size(); c0 += kBatch)
      {
         const int nb = static_cast<int>(std::min<size_t>(kBatch, cols.size() - c0));
         for (int b = 0; b < kBatch; ++b)
         {
            if (b >= nb)
            {
               inR[b] = zeroLine.data();
               inI[b] = zeroLine.data();
               continue;
            }
            const int m = cols[c0 + static_cast<size_t>(b)];
            double* lr = lineRe.data() + static_cast<size_t>(b) * M;
            double* li = lineIm.data() + static_cast<size_t>(b) * M;
            for (int n = 0; n < M; ++n)
            {
               lr[n] = pupilRe[static_cast<size_t>(n) * M + m];
               li[n] = pupilIm[static_cast<size_t>(n) * M + m];
            }
            inR[b] = lr;
            inI[b] = li;
         }
         plan.Apply<kBatch>(inR, inI, outR, outI, aRe.data(), aIm.data());
         for (int b = 0; b < nb; ++b)
         {
            const int m = cols[c0 + static_cast<size_t>(b)];
            for (int y = 0; y < ny; ++y)
            {
               midRe[static_cast<size_t>(y) * M + m] = outR[b][y];
               midIm[static_cast<size_t>(y) * M + m] = outI[b][y];
            }
         }
      }
      // Column pass: one CZT over kx per intermediate row -> |E|^2.
      std::vector<float>& slice = out[zi];
      slice.assign(static_cast<size_t>(nx) * ny, 0.0f);
      for (int y0 = 0; y0 < ny; y0 += kBatch)
      {
         const int nb = std::min(kBatch, ny - y0);
         for (int b = 0; b < kBatch; ++b)
         {
            const bool live = b < nb;
            inR[b] = live ? midRe.data() + static_cast<size_t>(y0 + b) * M : zeroLine.data();
            inI[b] = live ? midIm.data() + static_cast<size_t>(y0 + b) * M : zeroLine.data();
         }
         plan.Apply<kBatch>(inR, inI, outR, outI, aRe.data(), aIm.data());
         for (int b = 0; b < nb; ++b)
         {
            float* row = slice.data() + static_cast<size_t>(y0 + b) * nx;
            const double* r = outR[b];
            const double* i = outI[b];
            for (int x = 0; x < nx; ++x)
               row[x] = static_cast<float>(r[x] * r[x] + i[x] * i[x]);
         }
      }
   });
   return true;
}

ZernikeModeTable::ZernikeModeTable(const ZernikeCoefficients& coeffs)
{
   for (size_t j = 0; j < coeffs.size(); ++j)
   {
      if (coeffs[j] == 0.0)
         continue;
      Term t;
      t.coeff = coeffs[j];
      IndexToNM(static_cast<int>(j), t.n, t.l);
      t.m = std::abs(t.l);
      terms_.push_back(t);
   }
}

double ZernikeModeTable::Waves(double rho, double phi) const
{
   double w = 0.0;
   for (const Term& t : terms_)
   {
      ZernikeMode zm;
      zm.n = t.n;
      zm.l = t.l;
      zm.m = t.m;
      w += t.coeff * ZernikeValue(zm, rho, phi);
   }
   return w;
}

double ZernikeWavefrontWaves(const ZernikeCoefficients& coeffs, double rho, double phi)
{
   double w = 0.0;
   for (size_t j = 0; j < coeffs.size(); ++j)
   {
      if (coeffs[j] == 0.0)
         continue;
      ZernikeMode zm;
      IndexToNM(static_cast<int>(j), zm.n, zm.l);
      zm.m = std::abs(zm.l);
      w += coeffs[j] * ZernikeValue(zm, rho, phi);
   }
   return w;
}

bool BuildZernikePsfKernelCache(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError)
{
   outCache = PsfKernelCache();
   std::vector<std::vector<float>> planes;
   const auto tPlanes = TimingClock::now();
   if (!ComputeZernikePsfPlanes(req, planes, outError))
      return false;
   TimingLog("psf.zernike-planes", TimingSince(tPlanes));
   const auto tSums = TimingClock::now();
   const StackGeometry g = GeometryFor(req);
   outCache.oversampling = g.oversampling;
   outCache.halfWidthOversampled = g.halfOv;
   outCache.sizeOversampled = g.size;
   outCache.nz = g.nz;
   outCache.zStepNm = req.zStepNm;
   outCache.interpMode = req.interpMode;
   PsfKernelPlanes d;
   d.planes = std::move(planes);
   d.blockSums.assign(static_cast<size_t>(g.nz), std::vector<float>());
   outCache.blockSumWidth = g.size + g.oversampling - 1;
   ParallelFor(static_cast<unsigned>(g.nz), [&](unsigned z) {
      std::vector<float>& plane = d.planes[z];
      // Photon-normalize (sum 1), as the JVM path does.
      double sum = 0.0;
      for (float v : plane)
         sum += v;
      if (sum > 0.0)
         for (float& v : plane)
            v = static_cast<float>(v / sum);
      d.blockSums[z] = BuildBlockSums(plane.data(), g.size, g.oversampling);
   });
   BuildPolyphaseSums(d, outCache.blockSumWidth, g.oversampling);
   outCache.SetData(std::move(d));
   TimingLog("psf.normalize+block-sums", TimingSince(tSums));
   outCache.valid = true;
   return true;
}

} // namespace sim
