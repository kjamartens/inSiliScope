// ctest brightfield: the BrightField engine (Simulation/BrightfieldRender.*)
// against weak-phase-grating theory (thin and multislice), the empty field,
// and on the real world: energy, determinism (serial = parallel), caching.
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

#include "BrightfieldRender.h"
#include "CellFieldSource.h"
#include "Parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace sim;

namespace {

int g_failures = 0;
const double kPi = 3.14159265358979323846;

void Check(bool ok, const char* what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      g_failures++;
}

BrightfieldSpec GratingSpec()
{
   BrightfieldSpec s;
   s.width = s.height = 96;
   s.pixelUm = 0.1;
   s.upscale = 1;
   s.marginUm = 0.5;
   s.condenserNa = 0.0; // coherent, on axis
   s.sources = 1;
   s.na = 1.4;
   return s;
}

// A weak phase grating along x, total phase eps cos(g x), split over S
// slices of zTop / S. Image amplitude of cos(g x) against the first-order
// (Born) multislice prediction -2 (eps/S) sum_k sin(dkz (z_k - focus)).
void Grating(int S, double eps, double focus, double& got, double& want, double& dc)
{
   const BrightfieldSpec spec = GratingSpec();
   unsigned nx, ny, m0;
   BrightfieldScene::GridFor(spec, nx, ny, m0);
   const double pitch = spec.pixelUm, Lx = nx * pitch;
   const int m = static_cast<int>(std::lround(Lx / 1.0)); // ~1 um period
   const double g = 2 * kPi * m / Lx;
   const double zTop = 1.0, dz = zTop / S;
   std::vector<float> phase(static_cast<size_t>(nx) * ny * S);
   for (int k = 0; k < S; ++k)
      for (unsigned y = 0; y < ny; ++y)
         for (unsigned x = 0; x < nx; ++x)
            phase[(static_cast<size_t>(k) * ny + y) * nx + x] = static_cast<float>(eps / S * std::cos(g * x * pitch));
   BrightfieldScene scene;
   std::string err;
   std::vector<float> img;
   if (!scene.UpdateFromPhase(spec, S, zTop, phase, err) || !scene.Image(focus, img, err))
   {
      std::printf("  %s\n", err.c_str());
      got = want = dc = 1e9;
      return;
   }
   const double k0 = 2 * kPi / (spec.wavelengthNm * 1e-3), km = k0 * spec.nMedium;
   const double dkz = std::sqrt(km * km - g * g) - km;
   want = 0;
   for (int k = 0; k < S; ++k)
      want += -2 * (eps / S) * std::sin(dkz * ((k + 0.5) * dz - focus));
   double c = 0, cc = 0, mean = 0;
   for (unsigned j = 0; j < spec.height; ++j)
      for (unsigned i = 0; i < spec.width; ++i)
      {
         const double v = img[static_cast<size_t>(j) * spec.width + i];
         const double w = std::cos(g * (m0 + i) * pitch);
         c += (v - 1) * w;
         cc += w * w;
         mean += v;
      }
   got = c / cc;
   dc = mean / (static_cast<double>(spec.width) * spec.height);
}

CellFieldSettings World(double occupancy)
{
   CellFieldSettings cf;
   cf.seed = 42u ^ 0x43454C4Cu;
   cf.params = { { "density", occupancy } };
   return cf;
}

BrightfieldSpec FieldSpec(int quality)
{
   BrightfieldSpec s;
   s.width = s.height = 128;
   s.pixelUm = 0.1;
   s.originXUm = -6.4;
   s.originYUm = -6.4;
   s.quality = quality;
   return s;
}

} // namespace

int main()
{
   // Thin, coherent weak phase grating: contrast 2 eps sin(dkz d) with sign.
   {
      const double eps = 0.02;
      double worst = 0, inFocus = 1, dcErr = 0;
      for (double f : { 0.5, -0.5, 1.5, 2.5, -2.0 })
      {
         double got, want, dc;
         Grating(1, eps, f, got, want, dc);
         std::printf("      thin grating focus %+.1f: amplitude %+.5f, theory %+.5f\n", f, got, want);
         worst = std::max(worst, std::fabs(got - want));
         dcErr = std::max(dcErr, std::fabs(dc - 1));
         if (f == 0.5)
            inFocus = std::fabs(got);
      }
      Check(worst < 0.03 * 2 * eps + 2e-4, "thin weak phase grating = theory (sign and size, 3%)");
      Check(inFocus < 1e-4, "thin weak phase grating in focus: no contrast");
      Check(dcErr < 1e-3, "weak phase grating: mean intensity 1");
   }
   // Multislice: the slices' scattered waves add with their heights.
   {
      const double eps = 0.02;
      double worst = 0;
      for (double f : { 0.0, 2.0, -1.5 })
      {
         double got, want, dc;
         Grating(8, eps, f, got, want, dc);
         std::printf("      8-slice grating focus %+.1f: amplitude %+.5f, theory %+.5f\n", f, got, want);
         worst = std::max(worst, std::fabs(got - want));
      }
      Check(worst < 0.03 * 2 * eps + 2e-4, "8-slice weak phase grating = first-order multislice theory (3%)");
   }

   // The empty field: no cells, partially coherent: exactly 1 everywhere.
   {
      CellFieldSource src;
      std::string err;
      const bool cfgOk = src.Configure(World(0.0), err);
      BrightfieldScene scene;
      std::vector<float> img;
      double worst = 0;
      bool ok = cfgOk && scene.Update(src, FieldSpec(3), 1, err);
      for (double f : { 0.0, 3.0 })
      {
         ok = ok && scene.Image(f, img, err);
         for (float v : img)
            worst = std::max(worst, std::fabs(v - 1.0));
      }
      if (!ok)
         std::printf("  %s\n", err.c_str());
      Check(ok && worst < 1e-4, "empty field: transmitted intensity 1 at every pixel and focus");
   }

   // The real world (cells around the origin), every exposed quality level (1-4).
   {
      CellFieldSource src;
      std::string err;
      bool ok = src.Configure(World(0.33), err);
      for (int q = 1; q <= 4 && ok; ++q)
      {
         BrightfieldScene a;
         std::vector<float> img, again;
         ok = a.Update(src, FieldSpec(q), 1, err) && a.Image(0.5, img, err);
         if (!ok)
            break;
         double mean = 0, lo = 1e9, hi = -1e9;
         for (float v : img)
         {
            mean += v;
            lo = std::min(lo, static_cast<double>(v));
            hi = std::max(hi, static_cast<double>(v));
         }
         mean /= img.size();
         std::printf("      quality %d: grid %ux%u, %d sources, %d slices, setup %.0f ms, image %.0f ms, mean %.4f, "
                     "range [%.3f, %.3f]\n",
                     q, a.GridNx(), a.GridNy(), a.Sources(), a.Slices(), a.SetupMs(), a.LastImageMs(), mean, lo, hi);
         Check(std::fabs(mean - 1) < 0.03 && hi - lo > 0.02, "cells: mean intensity ~1 (energy), visible contrast");
         if (q == 3)
         {
            // Serial (inside a ParallelFor worker nothing nests) = parallel.
            BrightfieldScene b;
            ++ParallelDepth();
            ok = b.Update(src, FieldSpec(q), 1, err) && b.Image(0.5, again, err);
            --ParallelDepth();
            Check(ok && again == img, "serial = parallel, bit for bit");
            const auto t0 = std::chrono::steady_clock::now();
            ok = ok && a.Update(src, FieldSpec(q), 1, err) && a.Image(0.5, again, err);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Check(ok && again == img && ms < 50, "same spec and focus: cached");
            ok = ok && a.Image(-1.0, again, err);
            Check(ok && again != img, "a focus change changes the image");
         }
      }
      if (!ok)
         std::printf("  %s\n", err.c_str());
      Check(ok, "real world: every call succeeded");
   }

   std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall brightfield checks passed\n", g_failures);
   return g_failures ? 1 : 0;
}
