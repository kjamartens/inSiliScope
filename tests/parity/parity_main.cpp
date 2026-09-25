// Parity harness: reads tests/parity cases (see make_cases.mjs), runs them
// through core, prints results in the same line format js_reference.mjs
// prints for the JS prototype. Built natively and with Emscripten (run under
// Node with NODERAWFS), so the same file checks native, WASM and JS.
//
//   isc_parity <cases.txt> <out.txt>
#include "cells.h"
#include "jsmath.h"
#include "packing.h"
#include "params.h"
#include "rng.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace isc;

namespace {

uint32_t ToU32(long long v) { return (uint32_t)(int64_t)v; }   // JS `|0` then reinterpret

uint64_t Bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

void PrintD(FILE* f, double d) { std::fprintf(f, " %.17g", d); }

void MathCheck(FILE* f, int n)
{
   // Inputs from the hash so JS and C++ build them identically; four bands
   // exercise the small, medium and large (Payne-Hanek) argument reductions.
   static const double kScale[4] = { 8 * jsm::PI, 200, 1e5, 1e9 };
   for (int i = 0; i < n; i++) {
      const double u1 = HashUnit(7, i, 0, 0), u2 = HashUnit(7, i, 1, 0), u3 = HashUnit(7, i, 2, 0);
      const double s = kScale[i % 4];
      const double x = (u1 - 0.5) * 2 * s;
      const double ay = (u2 - 0.5) * s, ax = (u3 - 0.5) * s;
      std::fprintf(f, "m %d %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n", i,
                   Bits(jsm::sin(x)), Bits(jsm::cos(x)), Bits(jsm::atan2(ay, ax)), Bits(jsm::hypot(ay, ax)),
                   Bits(jsm::atan(x)), Bits(jsm::exp((u1 - 0.5) * 1500)), Bits(jsm::log(u2 * s)));
   }
}

} // namespace

int main(int argc, char** argv)
{
   if (argc < 3) { std::fprintf(stderr, "usage: isc_parity <cases.txt> <out.txt>\n"); return 2; }
   std::ifstream in(argv[1]);
   if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
   FILE* f = std::fopen(argv[2], "wb");   // LF on every platform
   if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }

   std::map<std::string, Params> paramSets;
   std::string line;
   while (std::getline(in, line)) {
      std::istringstream ss(line);
      std::string kind;
      ss >> kind;
      if (kind == "params") {
         std::string name, kv;
         ss >> name;
         Params p;
         while (ss >> kv) {
            const size_t eq = kv.find('=');
            const std::string key = kv.substr(0, eq);
            if (!SetParam(p, key.c_str(), std::stod(kv.substr(eq + 1)))) {
               std::fprintf(stderr, "unknown param %s\n", key.c_str());
               return 2;
            }
         }
         NormalizeParams(p);
         paramSets[name] = p;
      } else if (kind == "rng") {
         long long seed, cx, cy, k;
         ss >> seed >> cx >> cy >> k;
         const Pcg4dOut r = Pcg4d(ToU32(seed), ToU32(cx), ToU32(cy), ToU32(k));
         std::fprintf(f, "rng %lld %lld %lld %lld %u %u %u %u", seed, cx, cy, k, r.a, r.b, r.c, r.d);
         PrintD(f, HashUnit(ToU32(seed), (int32_t)ToU32(cx), (int32_t)ToU32(cy), ToU32(k)));
         std::fprintf(f, "\n");
      } else if (kind == "stream") {
         long long seed, cx, cy, base; int count;
         ss >> seed >> cx >> cy >> base >> count;
         HashStream hs(ToU32(seed), (int32_t)ToU32(cx), (int32_t)ToU32(cy), ToU32(base));
         std::fprintf(f, "stream %lld %lld %lld %lld", seed, cx, cy, base);
         for (int i = 0; i < count; i++) PrintD(f, hs.Next());
         std::fprintf(f, "\n");
      } else if (kind == "math") {
         int n; ss >> n;
         MathCheck(f, n);
      } else if (kind == "case") {
         std::string id, pname; long long seed; int cx0, cy0, cx1, cy1;
         ss >> id >> seed >> pname >> cx0 >> cy0 >> cx1 >> cy1;
         const Params& p = paramSets.at(pname);
         const uint32_t s = ToU32(seed);

         const auto t0 = std::chrono::steady_clock::now();
         CandidateMap m = BuildCandidateMap(s, cx0, cy0, cx1, cy1, p);
         std::string rawLines;
         for (const Cell& c : m.cells) {
            char buf[512];
            std::snprintf(buf, sizeof buf, "raw %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
                          c.cx, c.cy, c.x, c.y, c.semiMajor, c.semiMinor, c.rot, c.height, c.modFloor, c.nucZ, c.rOuter, c.priority);
            rawLines += buf;
         }
         const int removed = PackMap(m, p);
         const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

         std::fprintf(f, "case %s ncand %zu removed %d ms %.3f\n", id.c_str(), m.cells.size(), removed, ms);
         std::fputs(rawLines.c_str(), f);
         for (size_t i = 0; i < m.cells.size(); i++) {
            if (!m.alive[i]) continue;
            const Cell& c = m.cells[i];
            std::fprintf(f, "pk %d %d", c.cx, c.cy);
            PrintD(f, c.x); PrintD(f, c.y); PrintD(f, c.packRot);
            std::fprintf(f, "\n");
         }
      }
   }
   std::fclose(f);
   return 0;
}
