// live_bench: live frames of one spec through sim::LiveScope (the viewer's isc_live_frame, one frame per call), timed
// one by one -- the live speed of a scene (2026-11-11: SIM + WideField). Not a test.
//
//   live_bench [--frames N] [--dump file.raw] [--hash] [--zstep um [--zevery k]] spec options...
//                                                                           (e.g. size=256 sim-type=SIM2D ...)
// --zstep: the focus moves by um every k frames (default 1), up and back (a focus scrub).
//
// Prints ms per frame (each, then the mean over frames 1.. and over the last cycle-worth); --dump writes every frame's
// uint16 pixels one after another (to compare two builds); --hash prints a per-frame FNV-1a of the pixels.
// ISC_TIMING=1 / ISC_THREADS=1 apply as for the cli.
#include "LiveScope.h"
#include "ScopeMovie.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace sim;

int main(int argc, char** argv)
{
   long frames = 20;
   std::string dump, text;
   bool hash = false;
   double zstep = 0.0, z0 = 0.5;
   long zevery = 1;
   for (int i = 1; i < argc; ++i)
   {
      const std::string a = argv[i];
      if (a == "--frames" && i + 1 < argc)
         frames = std::atol(argv[++i]);
      else if (a == "--dump" && i + 1 < argc)
         dump = argv[++i];
      else if (a == "--hash")
         hash = true;
      else if (a == "--zstep" && i + 1 < argc)
         zstep = std::atof(argv[++i]);
      else if (a == "--zevery" && i + 1 < argc)
         zevery = std::max(1L, std::atol(argv[++i]));
      else
         text += a + " ";
   }
   ScopeSpec spec;
   std::string err;
   if (!ParseScopeSpec(text, spec, err))
   {
      std::fprintf(stderr, "spec: %s\n", err.c_str());
      return 2;
   }
   std::FILE* out = dump.empty() ? nullptr : std::fopen(dump.c_str(), "wb");
   LiveScope live;
   std::vector<double> ms;
   for (long f = 0; f < frames; ++f)
   {
      std::vector<uint16_t> adu;
      LiveFrameInfo info;
      if (zstep != 0.0)
      {
         // up for 10 steps, down for 10, ...
         const long k = f / zevery, ph = k % 20;
         const double z = z0 + zstep * (ph < 10 ? ph : 20 - ph);
         if (!ParseScopeSpec(text + " z=" + std::to_string(z), spec, err))
            return 2;
      }
      const auto t0 = std::chrono::steady_clock::now();
      if (!live.Frame(spec, true, adu, info, err))
      {
         std::fprintf(stderr, "frame %ld: %s\n", f, err.c_str());
         return 1;
      }
      ms.push_back(1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      if (out)
         std::fwrite(adu.data(), sizeof(uint16_t), adu.size(), out);
      uint64_t h = 1469598103934665603ull;
      for (uint16_t v : adu)
         h = (h ^ v) * 1099511628211ull;
      std::printf("frame %3ld  %8.1f ms", f, ms.back());
      if (hash)
         std::printf("  %016llx", static_cast<unsigned long long>(h));
      std::printf("\n");
   }
   if (out)
      std::fclose(out);
   double s = 0;
   for (size_t i = 1; i < ms.size(); ++i)
      s += ms[i];
   const size_t half = ms.size() / 2;
   double s2 = 0;
   for (size_t i = half; i < ms.size(); ++i)
      s2 += ms[i];
   std::printf("mean frames 1..%zu: %.1f ms; second half: %.1f ms\n", ms.size() - 1,
               ms.size() > 1 ? s / (ms.size() - 1) : 0.0, ms.size() > half ? s2 / (ms.size() - half) : 0.0);
   return 0;
}
