// SR render regression check (ctest sr_render): the splat, the Fft placement
// and the parallel frame paths against REFERENCE copies of the serial code
// they replaced (verbatim below, from before the 2026-09-28 speed-up), on a
// synthetic vectorial kernel. Every rendered pixel must be bit-identical.
#include "PsfGeneratorBridge.h"
#include "SMLMNoise.h"
#include "SMLMSimulation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace sim;

namespace ref {
double RoundHalfUp(double v) { return std::floor(v + 0.5); }

// In-place iterative radix-2 complex FFT; n a power of two; sign -1
// forward, +1 inverse, both unnormalized.
void Fft1d(double* re, double* im, int n, int stride, int sign)
{
   for (int i = 1, j = 0; i < n; ++i)
   {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      if (i < j)
      {
         std::swap(re[i * stride], re[j * stride]);
         std::swap(im[i * stride], im[j * stride]);
      }
   }
   for (int len = 2; len <= n; len <<= 1)
   {
      const double ang = sign * 2.0 * 3.14159265358979323846 / len;
      const double wr = std::cos(ang), wi = std::sin(ang);
      const int half = len / 2;
      for (int i = 0; i < n; i += len)
      {
         double cr = 1.0, ci = 0.0;
         for (int k = 0; k < half; ++k)
         {
            double* ar = re + (i + k) * stride;
            double* ai = im + (i + k) * stride;
            double* br = re + (i + k + half) * stride;
            double* bi = im + (i + k + half) * stride;
            const double vr = *br * cr - *bi * ci, vi = *br * ci + *bi * cr;
            *br = *ar - vr;
            *bi = *ai - vi;
            *ar += vr;
            *ai += vi;
            const double nr = cr * wr - ci * wi;
            ci = cr * wi + ci * wr;
            cr = nr;
         }
      }
   }
}

// Shifts one zero-padded complex line (length N, first n entries live) by
// `shift` samples via the Fourier shift theorem: g(i) = f(i+shift), wrapped
// (k >= N/2 -> k-N) frequencies. phase[k] = exp(+2*pi*i*kk*shift/N) is
// precomputed by the caller (one table per axis).
void FourierShiftLine(std::vector<double>& re, std::vector<double>& im, int N, const std::vector<double>& pc,
                      const std::vector<double>& ps)
{
   Fft1d(re.data(), im.data(), N, 1, -1);
   for (int k = 0; k < N; ++k)
   {
      const double r = re[k], i = im[k];
      re[k] = r * pc[k] - i * ps[k];
      im[k] = r * ps[k] + i * pc[k];
   }
   Fft1d(re.data(), im.data(), N, 1, 1);
   const double norm = 1.0 / N;
   for (int k = 0; k < N; ++k)
   {
      re[k] *= norm;
      im[k] *= norm;
   }
}

// Port of webSMLM's fftShiftKernelTile(): shifts a real n x n kernel tile by
// a continuous (shiftX, shiftY), in kernel-grid units, via the Fourier
// shift theorem on a zero-padded (>= 2x) buffer -- g(i) = f(i+shift),
// using wrapped (k >= N/2 -> k-N) frequencies. webSMLM does one N x N 2D
// FFT pair plus N^2 phase factors; the shift phase exp(i(kx sx + ky sy)) is
// separable, so this does the IDENTICAL linear operation as 1D shifts along
// every row and then every column (the intermediate kept complex, outputs
// outside the n x n tile never needed) -- the same result, ~N/n times less
// work and 2N instead of N^2 trig pairs. Still one transform per emitter,
// so Fft remains the slowest placement mode.
std::vector<float> FftShiftKernelTile(const float* kernel, int n, double shiftX, double shiftY)
{
   int N = 1;
   while (N < 2 * n)
      N <<= 1;
   const int half = N / 2;
   auto phaseTable = [&](double shift, std::vector<double>& pc, std::vector<double>& ps) {
      pc.resize(N);
      ps.resize(N);
      for (int k = 0; k < N; ++k)
      {
         const int kk = k < half ? k : k - N;
         const double ang = 2.0 * 3.14159265358979323846 * kk * shift / N;
         pc[k] = std::cos(ang);
         ps[k] = std::sin(ang);
      }
   };
   std::vector<double> xc, xs, yc, ys;
   phaseTable(shiftX, xc, xs);
   phaseTable(shiftY, yc, ys);

   // Row pass: n rows, each zero-padded to N; keep the first n (complex) outputs.
   std::vector<double> midRe(static_cast<size_t>(n) * n), midIm(static_cast<size_t>(n) * n);
   std::vector<double> re(N), im(N);
   for (int y = 0; y < n; ++y)
   {
      std::fill(re.begin(), re.end(), 0.0);
      std::fill(im.begin(), im.end(), 0.0);
      for (int x = 0; x < n; ++x)
         re[x] = kernel[static_cast<size_t>(y) * n + x];
      FourierShiftLine(re, im, N, xc, xs);
      for (int x = 0; x < n; ++x)
      {
         midRe[static_cast<size_t>(y) * n + x] = re[x];
         midIm[static_cast<size_t>(y) * n + x] = im[x];
      }
   }
   // Column pass on the complex intermediate; the real part is the answer.
   std::vector<float> out(static_cast<size_t>(n) * n);
   for (int x = 0; x < n; ++x)
   {
      std::fill(re.begin(), re.end(), 0.0);
      std::fill(im.begin(), im.end(), 0.0);
      for (int y = 0; y < n; ++y)
      {
         re[y] = midRe[static_cast<size_t>(y) * n + x];
         im[y] = midIm[static_cast<size_t>(y) * n + x];
      }
      FourierShiftLine(re, im, N, yc, ys);
      for (int y = 0; y < n; ++y)
         out[static_cast<size_t>(y) * n + x] = static_cast<float>(re[y]);
   }
   return out;
}


// The pre-2026-09-28 SplatPsfKernel, verbatim.
void SplatPsfKernel(std::vector<float>& img, unsigned width, unsigned height, const PsfKernelCache& cache,
                     int zIndex, double xPx, double yPx, double totalPhotons, PsfInterpMode interpMode)
{
   if (!cache.valid || totalPhotons <= 0.0)
      return;
   if (zIndex < 0 || zIndex >= cache.nz || cache.blockSums.size() != static_cast<size_t>(cache.nz))
      return;

   const int os = std::max(1, cache.oversampling);
   const int n = cache.sizeOversampled;
   const int camRad = cache.halfWidthOversampled / os;
   const int off = os - 1;
   const int bw = cache.blockSumWidth;

   SplatSetupResult st;
   std::vector<float> shiftedSums; // Fft mode only
   const float* B = cache.blockSums[static_cast<size_t>(zIndex)].data();
   if (interpMode == PsfInterpMode::Fft)
   {
      // Align the shared sub-cell fraction onto the grid with ONE Fourier
      // shift of the raw kernel, then read its block sums nearest.
      st = sim::SplatSetup(cache, xPx, yPx, PsfInterpMode::Nearest);
      const double kc = (n - 1) / 2.0;
      const double tx = kc + (st.x0 - xPx - 0.5) * os + 0.5, ty = kc + (st.y0 - yPx - 0.5) * os + 0.5;
      const double rx = RoundHalfUp(tx), ry = RoundHalfUp(ty);
      std::vector<float> shifted =
         FftShiftKernelTile(cache.planes[static_cast<size_t>(zIndex)].data(), n, tx - rx, ty - ry);
      shiftedSums = sim::BuildBlockSums(shifted.data(), n, os);
      B = shiftedSums.data();
      st.bx = static_cast<int>(rx);
      st.by = static_cast<int>(ry);
   }
   else
   {
      st = sim::SplatSetup(cache, xPx, yPx, interpMode);
   }

   const int nt = st.nTaps;
   for (int dy = -camRad; dy <= camRad; ++dy)
   {
      const int Y = st.y0 + dy;
      if (Y < 0 || Y >= static_cast<int>(height))
         continue;
      const int r0 = st.by + dy * os + off;
      float* rowOut = img.data() + static_cast<size_t>(Y) * width;
      for (int dx = -camRad; dx <= camRad; ++dx)
      {
         const int X = st.x0 + dx;
         if (X < 0 || X >= static_cast<int>(width))
            continue;
         const int c0 = st.bx + dx * os + off;
         double sum = 0.0;
         for (int j = 0; j < nt; ++j)
         {
            const int r = r0 + j;
            if (r < 0 || r >= bw)
               continue;
            const float* brow = B + static_cast<size_t>(r) * bw;
            double row = 0.0;
            for (int i = 0; i < nt; ++i)
            {
               const int c = c0 + i;
               if (c >= 0 && c < bw)
                  row += st.wx[i] * brow[c];
            }
            sum += st.wy[j] * row;
         }
         rowOut[X] += static_cast<float>(totalPhotons * sum);
      }
   }
}
} // namespace ref

namespace {

int g_failures = 0;
void Check(bool ok, const char* what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      ++g_failures;
}

PsfKernelCache SyntheticKernel(int os, int halfPx, int nz, PsfInterpMode mode)
{
   PsfKernelCache c;
   c.valid = true;
   c.oversampling = os;
   c.halfWidthOversampled = halfPx * os;
   c.sizeOversampled = 2 * c.halfWidthOversampled + 1;
   c.nz = nz;
   c.zStepNm = 100;
   c.interpMode = mode;
   const int n = c.sizeOversampled;
   const double kc = (n - 1) / 2.0;
   for (int z = 0; z < nz; ++z)
   {
      const double dz = (z - (nz - 1) / 2.0) * 0.1;
      const double sx = (1.0 + 0.8 * std::fabs(dz + 0.3)) * os, sy = (1.0 + 0.8 * std::fabs(dz - 0.3)) * os;
      std::vector<float> pl(static_cast<size_t>(n) * n);
      double s = 0;
      for (int y = 0; y < n; ++y)
         for (int x = 0; x < n; ++x)
         {
            const double X = x - kc, Y = y - kc, r = std::sqrt(X * X + Y * Y) / os;
            const double v = std::exp(-0.5 * (X * X / (sx * sx) + Y * Y / (sy * sy))) +
                             0.01 * std::exp(-0.5 * (r - 4) * (r - 4)) + 1e-6 / (1 + r * r);
            pl[static_cast<size_t>(y) * n + x] = static_cast<float>(v);
            s += v;
         }
      for (float& v : pl)
         v = static_cast<float>(v / s);
      c.blockSums.push_back(BuildBlockSums(pl.data(), n, os));
      c.planes.push_back(std::move(pl));
   }
   c.blockSumWidth = n + os - 1;
   return c;
}

bool SameFloats(const std::vector<float>& a, const std::vector<float>& b)
{
   return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

int main()
{
   const unsigned W = 48, H = 40;
   std::mt19937_64 rng(7);
   std::uniform_real_distribution<double> ux(-12.0, W + 12.0), uy(-12.0, H + 12.0), up(1.0, 2000.0);
   const PsfInterpMode modes[] = {PsfInterpMode::Nearest, PsfInterpMode::Linear, PsfInterpMode::Cubic,
                                  PsfInterpMode::Fft};
   const char* names[] = {"Nearest", "Linear", "Cubic", "Fft"};
   for (int m = 0; m < 4; ++m)
   {
      // Oversampling 4 and 5 (odd/even kernel grids), emitters on and off
      // the image and on its edges.
      bool same = true;
      for (int os : {4, 5})
      {
         const PsfKernelCache kc = SyntheticKernel(os, 8, 5, modes[m]);
         std::vector<float> a(static_cast<size_t>(W) * H, 1.5f), b = a;
         const int nEm = modes[m] == PsfInterpMode::Fft ? 40 : 400;
         for (int k = 0; k < nEm; ++k)
         {
            double x = ux(rng), y = uy(rng);
            if (k % 10 == 0)
               x = std::floor(x) + 0.5; // ties of the rounding
            const int z = k % 5;
            const double ph = up(rng);
            ref::SplatPsfKernel(a, W, H, kc, z, x, y, ph, modes[m]);
            SplatPsfKernel(b, W, H, kc, z, x, y, ph, modes[m]);
         }
         same = same && SameFloats(a, b);
      }
      char what[160];
      std::snprintf(what, sizeof what, "%s splat = the previous code, bit for bit", names[m]);
      Check(same, what);
   }

   // Whole frames: parallel bands (render) and rows (noise) = serial.
   std::vector<BlinkEvent> events;
   for (int k = 0; k < 300; ++k)
   {
      BlinkEvent e{ux(rng) * 0.1, uy(rng) * 0.1, (k % 9 - 4) * 100.0, k * 0.01 - 0.5, k * 0.01 + 0.7};
      e.brightness = k % 3 ? 1.0 : 0.8;
      events.push_back(e);
   }
   std::vector<float> illum(static_cast<size_t>(W) * H), bg(illum.size());
   for (size_t i = 0; i < illum.size(); ++i)
   {
      illum[i] = static_cast<float>(0.5 + 0.5 * std::cos(i * 0.01));
      bg[i] = static_cast<float>(2.0 + std::sin(i * 0.03));
   }
   std::mt19937_64 mrng(3);
   PixelOffsetMap om;
   om.Generate(W, H, 100, 0.5, mrng);
   PixelGainMap gm;
   gm.Generate(W, H, 0.25, 0.05, mrng);
   PixelReadNoiseMap rm;
   rm.Generate(W, H, 1.2, 0.2, mrng);
   for (int m = 0; m < 5; ++m)
   {
      const PsfKernelCache kc = SyntheticKernel(4, 8, 9, m < 4 ? modes[m] : PsfInterpMode::Cubic);
      bool same = true;
      for (int f = 0; f < 3; ++f)
      {
         RenderExtras ex;
         if (f > 0)
         {
            ex.illumField = &illum;
            ex.backgroundMap = f > 1 ? &bg : nullptr;
            ex.backgroundScale = 0.9;
         }
         RenderExtras px = ex;
         px.parallel = true;
         const PsfKernelCache* cache = m < 4 ? &kc : nullptr; // m = 4: the Gaussian PSF
         std::vector<float> a, b;
         long za = 0, ta = 0, zb = 0, tb = 0;
         RenderPhotonImage(a, W, H, events, f, 100, 1.3, 400, 2.5, 0.3, -0.2, cache, 0.2, &za, &ta, &ex);
         RenderPhotonImage(b, W, H, events, f, 100, 1.3, 400, 2.5, 0.3, -0.2, cache, 0.2, &zb, &tb, &px);
         same = same && SameFloats(a, b) && za == zb && ta == tb;
         for (bool emccd : {false, true})
         {
            CameraNoiseParams cam;
            cam.emccd = emccd;
            std::vector<uint16_t> na, nb;
            ApplyNoiseChain(a, na, W, H, cam, om, gm, rm, 99u, static_cast<uint32_t>(f));
            ApplyNoiseChain(a, nb, W, H, cam, om, gm, rm, 99u, static_cast<uint32_t>(f), true);
            same = same && na == nb;
         }
      }
      char what[160];
      std::snprintf(what, sizeof what, "%s frames: parallel render + noise = serial", m < 4 ? names[m] : "Gaussian");
      Check(same, what);
   }
   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall SR render checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
