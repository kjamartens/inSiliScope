// Copyright (c) 2026 insiliscope contributors
// SPDX-License-Identifier: BSD-3-Clause
// ctest csv_numbers (native and WASM): the ground truth tables' numbers (GroundTruth.cpp AppendCsvNumber, by
// std::to_chars) = snprintf's into 48 bytes, for every format the tables use, on ~2 M values (random magnitudes,
// exact binary fractions, a few ulps either side of every precision's rounding boundaries, +-0, huge and tiny), and
// NaN / infinities empty. In WASM: libc++'s to_chars against musl's printf.
//   csv_number_check          exit code 0 = passed
#include "GroundTruth.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace sim;

int main()
{
   bool ok = false;
   // ---- the tables' numbers: AppendCsvNumber (std::to_chars) = snprintf into 48 bytes, for every format used ----
   {
      uint64_t st = 0x9E3779B97F4A7C15ull;
      const auto next = [&]() {   // splitmix64
         uint64_t z = (st += 0x9E3779B97F4A7C15ull);
         z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
         z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
         return z ^ (z >> 31);
      };
      std::vector<double> v = { 0.0, -0.0, 1e-300, -1e-300, 4.9e-324, 1e-9, -1e-9, 0.5, 1.5, 2.5, -2.5, 0.125, 0.0625,
                                0.03125, 1e15, 1e20, 1e22, 1e40, -1e40, 1e300, 123456.5, 999999.5, 9999.5, 0.00049999,
                                0.0005, 0.00005, 2e-5, 1e-4 };
      for (int i = 0; i < 100000; ++i)
      {
         const uint64_t r = next();
         const double u = static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);   // [0, 1)
         const int e = static_cast<int>(next() % 25) - 12;
         v.push_back((r & 1 ? -1 : 1) * u * std::pow(10.0, e));
         // exact binary fractions (finite decimals: ties at some precisions)
         v.push_back(static_cast<double>(static_cast<int64_t>(next() % 2000001) - 1000000) / static_cast<double>(1ull << (next() % 31)));
         // rounding boundaries of every precision, a few ulps either side
         const int p = static_cast<int>(next() % 9);
         const double mid = (static_cast<double>(next() % 2000000) + 0.5) * std::pow(10.0, -p);
         double m = mid;
         for (int k = 0; k < 3; ++k)
            m = std::nextafter(m, 0.0);
         for (int k = 0; k < 7; ++k, m = std::nextafter(m, 1e300))
            v.push_back(m);
      }
      const char* fmts[] = { "%.6f", "%.4f", "%.3f", "%.6g", "%.4g", "%.9g" };
      long n = 0, bad = 0;
      std::string first;
      for (double x : v)
         for (const char* f : fmts)
         {
            char c[48];
            std::snprintf(c, sizeof c, f, x);
            std::string got;
            AppendCsvNumber(got, x, f);
            ++n;
            if (got != c)
            {
               if (!bad++)
                  first = std::string(f) + " of " + std::to_string(x) + ": " + got + " vs " + c;
            }
         }
      std::string nf;
      AppendCsvNumber(nf, std::numeric_limits<double>::quiet_NaN(), "%.4f");
      AppendCsvNumber(nf, std::numeric_limits<double>::infinity(), "%.4f");
      ok = bad == 0 && nf.empty();
      const std::string what = "the tables' numbers (std::to_chars) = snprintf's, " + std::to_string(n) +
                               " values x formats" +
                               (bad ? " (" + std::to_string(bad) + " differ, first: " + first + ")" : "") +
                               "; NaN and infinities empty";
      std::printf("%s    %s\n", ok ? "ok" : "FAIL", what.c_str());
   }

   return ok ? 0 : 1;
}
