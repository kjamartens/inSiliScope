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

// Bluestein chirp-Z: out[p] = sum_m in[m] exp(i k_m x_p), k_m = k0 + m dk,
// x_p = x0 + p dx, m < M, p < P (psfCzt1d), with every input-independent
// factor tabulated.
struct CztPlan
{
   int M = 0, P = 0, L = 0;
   FftTwiddles fwd, inv;
   std::vector<double> preC, preS; // exp(i*(m*dk*x0 + theta*m*m/2))
   std::vector<double> gRe, gIm;   // FFT of the filter exp(-i*theta*n*n/2)
   std::vector<double> c1, s1;     // exp(i*theta*p*p/2)
   std::vector<double> c2, s2;     // exp(i*k0*(x0+p*dx))

   CztPlan(int M_, double dk, double k0, int P_, double dx, double x0) : M(M_), P(P_)
   {
      const double theta = dk * dx;
      L = 1;
      while (L < M + P - 1)
         L <<= 1;
      fwd = MakeTwiddles(L, -1);
      inv = MakeTwiddles(L, 1);
      preC.resize(static_cast<size_t>(M));
      preS.resize(static_cast<size_t>(M));
      for (int m = 0; m < M; ++m)
      {
         const double ang = m * dk * x0 + theta * m * m / 2.0;
         preC[static_cast<size_t>(m)] = std::cos(ang);
         preS[static_cast<size_t>(m)] = std::sin(ang);
      }
      gRe.assign(static_cast<size_t>(L), 0.0);
      gIm.assign(static_cast<size_t>(L), 0.0);
      for (int n = -(M - 1); n < P; ++n)
      {
         const double ang = -theta * n * n / 2.0;
         const int idx = n >= 0 ? n : L + n;
         gRe[static_cast<size_t>(idx)] = std::cos(ang);
         gIm[static_cast<size_t>(idx)] = std::sin(ang);
      }
      Fft1d(gRe.data(), gIm.data(), fwd);
      c1.resize(static_cast<size_t>(P));
      s1.resize(c1.size());
      c2.resize(c1.size());
      s2.resize(c1.size());
      for (int p = 0; p < P; ++p)
      {
         const double a1 = theta * p * p / 2.0;
         c1[static_cast<size_t>(p)] = std::cos(a1);
         s1[static_cast<size_t>(p)] = std::sin(a1);
         const double a2 = k0 * (x0 + p * dx);
         c2[static_cast<size_t>(p)] = std::cos(a2);
         s2[static_cast<size_t>(p)] = std::sin(a2);
      }
   }

   // aRe/aIm: scratch of length L.
   void Apply(const double* inRe, const double* inIm, double* outRe, double* outIm, double* aRe, double* aIm) const
   {
      std::fill(aRe, aRe + L, 0.0);
      std::fill(aIm, aIm + L, 0.0);
      for (int m = 0; m < M; ++m)
      {
         const double cr = preC[static_cast<size_t>(m)], ci = preS[static_cast<size_t>(m)];
         aRe[m] = inRe[m] * cr - inIm[m] * ci;
         aIm[m] = inRe[m] * ci + inIm[m] * cr;
      }
      Fft1d(aRe, aIm, fwd);
      for (int i = 0; i < L; ++i)
      {
         const double gr = gRe[static_cast<size_t>(i)], gi = gIm[static_cast<size_t>(i)];
         const double re = aRe[i] * gr - aIm[i] * gi;
         const double im = aRe[i] * gi + aIm[i] * gr;
         aRe[i] = re;
         aIm[i] = im;
      }
      Fft1d(aRe, aIm, inv);
      for (int p = 0; p < P; ++p)
      {
         const double convRe = aRe[p] / L, convIm = aIm[p] / L;
         const double cc1 = c1[static_cast<size_t>(p)], ss1 = s1[static_cast<size_t>(p)];
         const double sRe = convRe * cc1 - convIm * ss1, sIm = convRe * ss1 + convIm * cc1;
         const double cc2 = c2[static_cast<size_t>(p)], ss2 = s2[static_cast<size_t>(p)];
         outRe[p] = sRe * cc2 - sIm * ss2;
         outIm[p] = sRe * ss2 + sIm * cc2;
      }
   }
};

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

      std::vector<double> aRe(static_cast<size_t>(plan.L)), aIm(aRe.size());
      // Row pass: one CZT over ky per pupil column m -> M x ny intermediate.
      std::vector<double> midRe(static_cast<size_t>(M) * ny, 0.0), midIm(midRe.size(), 0.0);
      std::vector<double> lineRe(static_cast<size_t>(M)), lineIm(lineRe.size());
      std::vector<double> oRe(static_cast<size_t>(std::max(nx, ny))), oIm(oRe.size());
      for (int m = 0; m < M; ++m)
      {
         bool any = false;
         for (int n = 0; n < M; ++n)
         {
            lineRe[static_cast<size_t>(n)] = pupilRe[static_cast<size_t>(n) * M + m];
            lineIm[static_cast<size_t>(n)] = pupilIm[static_cast<size_t>(n) * M + m];
            any = any || inside[static_cast<size_t>(n) * M + m];
         }
         if (!any)
            continue; // an all-zero column transforms to exactly zero
         plan.Apply(lineRe.data(), lineIm.data(), oRe.data(), oIm.data(), aRe.data(), aIm.data());
         for (int y = 0; y < ny; ++y)
         {
            midRe[static_cast<size_t>(y) * M + m] = oRe[static_cast<size_t>(y)];
            midIm[static_cast<size_t>(y) * M + m] = oIm[static_cast<size_t>(y)];
         }
      }
      // Column pass: one CZT over kx per intermediate row -> |E|^2.
      std::vector<float>& slice = out[zi];
      slice.assign(static_cast<size_t>(nx) * ny, 0.0f);
      for (int y = 0; y < ny; ++y)
      {
         plan.Apply(midRe.data() + static_cast<size_t>(y) * M, midIm.data() + static_cast<size_t>(y) * M, oRe.data(),
                    oIm.data(), aRe.data(), aIm.data());
         float* row = slice.data() + static_cast<size_t>(y) * nx;
         for (int x = 0; x < nx; ++x)
            row[x] = static_cast<float>(oRe[static_cast<size_t>(x)] * oRe[static_cast<size_t>(x)] +
                                        oIm[static_cast<size_t>(x)] * oIm[static_cast<size_t>(x)]);
      }
   });
   return true;
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
   outCache.planes = std::move(planes);
   outCache.blockSums.assign(static_cast<size_t>(g.nz), std::vector<float>());
   outCache.blockSumWidth = g.size + g.oversampling - 1;
   ParallelFor(static_cast<unsigned>(g.nz), [&](unsigned z) {
      std::vector<float>& plane = outCache.planes[z];
      // Photon-normalize (sum 1), as the JVM path does.
      double sum = 0.0;
      for (float v : plane)
         sum += v;
      if (sum > 0.0)
         for (float& v : plane)
            v = static_cast<float>(v / sum);
      outCache.blockSums[z] = BuildBlockSums(plane.data(), g.size, g.oversampling);
   });
   BuildPolyphaseSums(outCache);
   TimingLog("psf.normalize+block-sums", TimingSince(tSums));
   outCache.valid = true;
   return true;
}

} // namespace sim
