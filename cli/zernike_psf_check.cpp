// ctest zernike_psf: the C++ GibsonLanniZernike PSF (Simulation/ZernikePsf.cpp)
// against webSMLM's own JS (tests/psf/zernike_ref.bin, written by
// `node tools/psf_parity_check/dump_websmlm.mjs --fixture`): the four cases of
// tools/psf_parity_check/README.md, every plane of a 3-plane stack, relative
// L2 < 1e-6 and the same argmax. Also checks BuildZernikePsfKernelCache's
// normalization and block sums.
//   zernike_psf_check <zernike_ref.bin> [--bench | --stack <jvm dump> mixed|doubleHelix]
// --bench times the adapter's default stack (841 x 841 x 71) and prints it.
// --stack compares that stack, every plane, with the JVM's
// (`java DumpJavaPsf --stack`, tools/psf_parity_check/README.md; manual,
// Windows with the PSFGenerator jar).
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

#include "SMLMZernike.h"
#include "ZernikePsf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void Check(bool ok, const std::string& what)
{
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
   if (!ok)
      ++g_fail;
}

sim::PsfGeneratorRequest FixtureRequest();

// The adapter's default request (BuildPsfGeneratorRequest at its defaults):
// 70 px half width x os 6, 71 planes, MixedRealisticObjective (or the
// double-helix mask alone).
sim::PsfGeneratorRequest AdapterDefaultRequest(bool doubleHelix)
{
   sim::PsfGeneratorRequest r = FixtureRequest();
   r.oversampling = 6;
   r.kernelHalfWidthPx = 70;
   r.nz = 71;
   r.zernikeCoefficients = sim::FormatZernikeCoefficients(
      doubleHelix ? sim::ZeroZernikeCoefficients() : sim::ZernikePresetCoefficients("MixedRealisticObjective"), ',');
   r.maskType = doubleHelix ? sim::PsfMaskType::DoubleHelix : sim::PsfMaskType::None;
   return r;
}

std::vector<char> ReadAll(const char* path)
{
   std::ifstream f(path, std::ios::binary);
   return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string Coeffs(std::initializer_list<std::pair<int, double>> terms)
{
   sim::ZernikeCoefficients c = sim::ZeroZernikeCoefficients();
   for (const auto& t : terms)
      c[static_cast<size_t>(t.first)] = t.second;
   return sim::FormatZernikeCoefficients(c, ',');
}

// The fixture's grid: 65 x 65 at 25 nm = pixel 100 nm x oversampling 4, half
// width 8 px; 3 planes 100 nm apart; NA 1.4, 660 nm, ni 1.518, ti0 150 um.
sim::PsfGeneratorRequest FixtureRequest()
{
   sim::PsfGeneratorRequest r;
   r.model = sim::PsfModelKind::GibsonLanniZernike;
   r.wavelengthNm = 660.0;
   r.na = 1.4;
   r.immersionIndex = 1.518;
   r.sampleIndex = 1.518;
   r.workingDistanceUm = 150.0;
   r.sampleDepthNm = 0.0;
   r.pixelSizeNm = 100.0;
   r.oversampling = 4;
   r.kernelHalfWidthPx = 8;
   r.nz = 3;
   r.zStepNm = 100.0;
   r.maskModes = 5;
   r.maskWaist = 1.0;
   return r;
}

} // namespace

int main(int argc, char** argv)
{
   if (argc < 2)
   {
      std::fprintf(stderr, "usage: zernike_psf_check <zernike_ref.bin> [--bench]\n");
      return 2;
   }
   const std::vector<char> bytes = ReadAll(argv[1]);
   if (bytes.size() < 20 || std::memcmp(bytes.data(), "ZPSF", 4) != 0)
   {
      std::fprintf(stderr, "bad fixture %s\n", argv[1]);
      return 2;
   }
   int32_t hdr[4];
   std::memcpy(hdr, bytes.data() + 4, sizeof(hdr));
   const int cases = hdr[0], nx = hdr[1], ny = hdr[2], nz = hdr[3];
   const size_t plane = static_cast<size_t>(nx) * ny;
   if (bytes.size() != 20 + static_cast<size_t>(cases) * nz * plane * 4 || nx != 65 || ny != 65 || nz != 3)
   {
      std::fprintf(stderr, "fixture size/shape mismatch\n");
      return 2;
   }
   const float* ref = reinterpret_cast<const float*>(bytes.data() + 20); // little-endian hosts only

   // Keep in step with CASES in dump_websmlm.mjs.
   struct Case
   {
      const char* name;
      double ns, depthNm;
      std::string zernike;
      bool doubleHelix;
   };
   const Case list[] = {
      {"AstigmatismModerate", 1.518, 0.0, Coeffs({{5, 0.15}}), false},
      {"ExtendedRangeStrong", 1.518, 0.0, Coeffs({{5, 1.8}, {13, 0.8}, {25, 0.3}}), false},
      {"DoubleHelix", 1.518, 0.0, Coeffs({}), true},
      {"ns 1.33, 500 nm deep", 1.33, 500.0, Coeffs({}), false},
   };
   if (cases != 4)
   {
      std::fprintf(stderr, "fixture has %d cases, expected 4\n", cases);
      return 2;
   }

   for (int c = 0; c < cases; ++c)
   {
      sim::PsfGeneratorRequest r = FixtureRequest();
      r.sampleIndex = list[c].ns;
      r.sampleDepthNm = list[c].depthNm;
      r.zernikeCoefficients = list[c].zernike;
      r.maskType = list[c].doubleHelix ? sim::PsfMaskType::DoubleHelix : sim::PsfMaskType::None;
      std::vector<std::vector<float>> planes;
      std::string err;
      const bool ok = sim::ComputeZernikePsfPlanes(r, planes, err);
      Check(ok && planes.size() == 3 && planes[0].size() == plane, std::string(list[c].name) + ": computed " + err);
      if (!ok || planes.size() != 3)
         continue;
      for (int z = 0; z < nz; ++z)
      {
         const float* want = ref + (static_cast<size_t>(c) * nz + z) * plane;
         const std::vector<float>& got = planes[static_cast<size_t>(z)];
         double num = 0.0, den = 0.0;
         size_t amG = 0, amW = 0;
         for (size_t i = 0; i < plane; ++i)
         {
            const double d = static_cast<double>(got[i]) - want[i];
            num += d * d;
            den += static_cast<double>(want[i]) * want[i];
            if (got[i] > got[amG])
               amG = i;
            if (want[i] > want[amW])
               amW = i;
         }
         const double rel = std::sqrt(num / den);
         char buf[160];
         std::snprintf(buf, sizeof(buf), "%s plane %d: relative L2 %.3g vs webSMLM JS, argmax %zu/%zu", list[c].name, z,
                       rel, amG, amW);
         Check(rel < 1e-6 && amG == amW, buf);
      }
   }

   // The cache: sum-1 planes and block sums whose total is os^2 (every
   // sub-cell is counted once per block that covers it).
   {
      sim::PsfGeneratorRequest r = FixtureRequest();
      r.zernikeCoefficients = list[0].zernike;
      sim::PsfKernelCache cache;
      std::string err;
      const bool ok = sim::BuildZernikePsfKernelCache(r, cache, err);
      Check(ok && cache.valid && cache.nz == 3 && cache.sizeOversampled == 65 && cache.halfWidthOversampled == 32 &&
               cache.blockSumWidth == 68,
            "kernel cache geometry " + err);
      if (ok)
      {
         double s = 0.0, b = 0.0;
         for (float v : cache.planes[1])
            s += v;
         for (float v : cache.blockSums[1])
            b += v;
         Check(std::fabs(s - 1.0) < 1e-5 && std::fabs(b - 16.0) < 1e-3, "kernel cache planes sum to 1, block sums to os^2");
      }
   }

   if (argc > 2 && std::strcmp(argv[2], "--bench") == 0)
   {
      sim::PsfKernelCache cache;
      std::string err;
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = sim::BuildZernikePsfKernelCache(AdapterDefaultRequest(false), cache, err);
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::printf("bench: %d x %d x %d kernel in %.2f s (%s)\n", cache.sizeOversampled, cache.sizeOversampled, cache.nz,
                  s, ok ? "ok" : err.c_str());
   }
   if (argc > 4 && std::strcmp(argv[2], "--stack") == 0)
   {
      const std::vector<char> jvm = ReadAll(argv[3]);
      const bool dh = std::strcmp(argv[4], "doubleHelix") == 0;
      std::vector<std::vector<float>> planes;
      std::string err;
      const bool ok = sim::ComputeZernikePsfPlanes(AdapterDefaultRequest(dh), planes, err);
      const size_t n = 841, pl = n * n, nzs = 71;
      if (!ok || jvm.size() != 20 + nzs * pl * 4 || planes.size() != nzs)
         Check(false, "JVM stack dump readable and C++ stack computed " + err);
      else
      {
         const float* want = reinterpret_cast<const float*>(jvm.data() + 20);
         double worst = 0.0;
         int bad = 0;
         for (size_t z = 0; z < nzs; ++z)
         {
            double num = 0.0, den = 0.0;
            for (size_t i = 0; i < pl; ++i)
            {
               const double d = static_cast<double>(planes[z][i]) - want[z * pl + i];
               num += d * d;
               den += static_cast<double>(want[z * pl + i]) * want[z * pl + i];
            }
            const double rel = std::sqrt(num / den);
            worst = std::max(worst, rel);
            bad += rel < 1e-6 ? 0 : 1;
         }
         char buf[160];
         std::snprintf(buf, sizeof(buf), "%s adapter-default stack vs JVM: worst plane relative L2 %.3g (%d planes >= 1e-6)",
                       argv[4], worst, bad);
         Check(bad == 0, buf);
      }
   }

   std::printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
   return g_fail ? 1 : 0;
}
