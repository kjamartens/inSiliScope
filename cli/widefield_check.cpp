// ctest widefield: the WideField modality's pieces (WidefieldRender.h,
// Fft2d.h, Illumination.h) against direct references.
//
//   widefield_check          exit code 0 = all checks passed
#include "CellFieldSource.h"
#include "Fft2d.h"
#include "Illumination.h"
#include "WidefieldRender.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;

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
   double m = 0.0, peak = 0.0;
   for (size_t i = 0; i < a.size(); i++) {
      m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
      peak = std::max(peak, std::fabs(static_cast<double>(b[i])));
   }
   return peak > 0 ? m / peak : m;
}

// A PSF whose width depends on z, so the plane blending is exercised.
class TestPsf : public WidefieldPsf
{
public:
   explicit TestPsf(double pitch) : pitch_(pitch) {}
   double PlaneCoord(double defocusUm) const override { return defocusUm / 0.1; }
   int MinPlane() const override { return -30; }
   int MaxPlane() const override { return 30; }
   int Radius(int, int) const override { return 6; }
   void Kernel(int p, int R, std::vector<float>& out) const override
   {
      const int D = 2 * R + 1;
      const double s = (0.12 + 0.02 * std::abs(p)) / pitch_;
      out.assign(static_cast<size_t>(D) * D, 0.0f);
      double sum = 0;
      for (int y = -R; y <= R; y++)
         for (int x = -R; x <= R; x++) sum += std::exp(-(x * x + y * y) / (2 * s * s));
      for (int y = -R; y <= R; y++)
         for (int x = -R; x <= R; x++)
            out[static_cast<size_t>(y + R) * D + x + R] = static_cast<float>(std::exp(-(x * x + y * y) / (2 * s * s)) / sum);
   }

private:
   double pitch_;
};

WidefieldSceneSpec BaseSpec(unsigned W, int upscale)
{
   WidefieldSceneSpec s;
   s.originXUm = 3.0;
   s.originYUm = -2.0;
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
   const unsigned N = 16;
   Fft2d fft(N);
   std::mt19937 rng(7);
   std::uniform_real_distribution<float> U(-1, 1);
   std::vector<cfloat> x(N * N), X(N * N);
   for (auto& v : x) v = cfloat(U(rng), U(rng));
   X = x;
   fft.Forward(X.data());
   double err = 0, peak = 0;
   const double pi = 3.14159265358979323846;
   for (unsigned ky = 0; ky < N; ky++)
      for (unsigned kx = 0; kx < N; kx++) {
         std::complex<double> s = 0;
         for (unsigned y = 0; y < N; y++)
            for (unsigned xx = 0; xx < N; xx++)
               s += std::complex<double>(x[y * N + xx]) * std::polar(1.0, -2 * pi * (double(kx * xx) / N + double(ky * y) / N));
         err = std::max(err, std::abs(s - std::complex<double>(X[ky * N + kx])));
         peak = std::max(peak, std::abs(s));
      }
   Check(err / peak < 1e-5, "FFT vs naive DFT (rel. err < 1e-5)");
   std::vector<cfloat> y = X;
   fft.Inverse(y.data());
   double rt = 0;
   for (size_t i = 0; i < x.size(); i++) rt = std::max(rt, (double)std::abs(y[i] - x[i]));
   Check(rt < 1e-5, "FFT inverse round trip");
}

// Synthetic dye grid: random counts on a few planes around the focus.
WidefieldDyeGrid RandomGrid(const WidefieldGridSpec& g, bool bleach, bool persist, unsigned seed)
{
   WidefieldDyeGrid G;
   G.spec = g;
   G.k0 = static_cast<long>(std::floor(0.6 / g.zPlaneUm));
   G.nz = 32;
   const size_t n = static_cast<size_t>(g.nx) * g.ny * G.nz;
   std::mt19937 rng(seed);
   std::uniform_int_distribution<int> D(0, 30);
   if (bleach) G.bleaching.resize(n);
   if (persist) G.persistent.resize(n);
   for (size_t i = 0; i < n; i++) {
      const bool on = D(rng) == 0;
      if (bleach) { G.bleaching[i] = on ? (float)(1 + D(rng) % 3) : 0.0f; G.nBleaching += (long)G.bleaching[i]; }
      if (persist) { G.persistent[i] = D(rng) == 1 ? 1.0f : 0.0f; G.nPersistent += (long)G.persistent[i]; }
   }
   return G;
}

// Direct convolution reference: each dye plane to PSF planes p0, p0+1 with
// linear weights, convolve, crop the FOV, clamp, bin.
std::vector<float> DirectImage(const WidefieldDyeGrid& G, const WidefieldSceneSpec& s, const WidefieldPsf& psf,
                               const std::vector<float>& wb, const std::vector<float>& wp, unsigned mx, unsigned my)
{
   const WidefieldGridSpec& g = G.spec;
   const size_t n2 = static_cast<size_t>(g.nx) * g.ny;
   const int u = s.grid.upscale, R = 6, D = 2 * R + 1;
   std::vector<double> img(n2, 0.0);
   std::vector<float> k;
   for (unsigned i = 0; i < G.nz; i++) {
      const double t = psf.PlaneCoord(G.PlaneCentreUm(i) - s.focusWorldUm);
      const int p0 = (int)std::floor(t);
      const double fr = t - p0;
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
            for (int sx = 0; sx < u; sx++) acc += std::max(0.0, img[(size_t)(my + Y * u + sy) * g.nx + mx + X * u + sx]);
         cam[(size_t)Y * s.width + X] = (float)acc;
      }
   return cam;
}

void ConvolutionVsDirect()
{
   for (int u : {1, 2}) {
      WidefieldSceneSpec s = BaseSpec(24, u);
      // Illumination wider than the FOV: exercises the margin.
      SquareIllumination ill(3.0, 2.8);
      unsigned mx = 0, my = 0;
      const WidefieldGridSpec g = WidefieldScene::GridSpecFor(ill, s, &mx, &my);
      TestPsf psf(g.pitchUm);
      WidefieldDyeGrid G = RandomGrid(g, true, true, 11 + u);
      WidefieldScene sc;
      std::string err;
      const bool ok = sc.UpdateFromGrid(G, ill, s, psf, err);
      std::vector<float> wb, cam;
      sc.FreshBleachWeights(3, wb);
      sc.RenderFrame(wb, cam);
      std::vector<float> wp(sc.FrameDose().size());
      for (size_t i = 0; i < wp.size(); i++) wp[i] = (float)(s.eta * sc.FrameDose()[i]);
      const std::vector<float> ref = DirectImage(G, s, psf, wb, wp, mx, my);
      const double d = MaxRelDiff(cam, ref);
      char msg[160];
      std::snprintf(msg, sizeof msg, "FFT convolution == direct convolution, upscale %d, margin %u (max rel diff %.2e)", u, mx, d);
      Check(ok && mx > 0 && d < 1e-4, msg);
   }
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
   Check(std::fabs(t - 30.0) < 0.05 && std::fabs(perFrame - 1.8) < 0.05 && std::fabs(eta - 0.307) < 0.001, msg);
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
   std::vector<cfloat> scratch;
   full.RenderScaled(1.0, c, scratch);
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

void RealWorld()
{
   CellFieldSource src;
   CellFieldSettings cf;
   cf.seed = 42 ^ 0x43454C4Cu;
   cf.params = { { "labelEfficiency", 0.2 }, { "labelNonBleaching", 0.5 } };
   std::string err;
   if (!src.Configure(cf, err)) { Check(false, err.c_str()); return; }
   WidefieldSceneSpec s = BaseSpec(48, 1);
   s.originXUm = -4 - 2.4;
   s.originYUm = -5 - 2.4;
   s.focusWorldUm = s.slabCentreUm = 0.5;
   s.slabHalfUm = 3.5;
   SquareIllumination ill(4.8, 4.8);
   GaussianWidefieldPsf psf(0.1, 660, 1.4, 1.518);
   WidefieldScene sc;
   const bool ok = sc.Update(src, ill, s, psf, err);
   std::vector<float> wb, cam;
   sc.FreshBleachWeights(0, wb);
   sc.RenderFrame(wb, cam);
   char msg[160];
   std::snprintf(msg, sizeof msg, "cell field: %ld dyes (%ld bleaching), %u^2 FFT, R %d, %d PSF planes, %.0f photons",
                 sc.Dyes(), sc.BleachingDyes(), sc.FftSize(), sc.KernelRadius(), sc.PsfPlanes(), Sum(cam));
   Check(ok && sc.Dyes() > 1000 && sc.BleachingDyes() > 0 && Sum(cam) > 0, msg);
}

} // namespace

int main()
{
   FftVsDft();
   ConvolutionVsDirect();
   PhotonConservation();
   Units();
   FrameIntegral();
   SplitRespected();
   Illumination();
   FastEqualsFull();
   RealWorld();
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall widefield checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
