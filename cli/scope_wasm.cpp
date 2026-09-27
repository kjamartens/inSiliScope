// WASM exports of the scope movie (sim::RenderScopeMovie) for the viewer:
// the same render code as the inSiliScope camera and insiliscope_cli, in
// the viewer's module next to the core's C ABI.
#include "ScopeMovie.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern "C" {

// Named options, one per line: name \t default \t help. Returns the length
// (without the terminating 0); writes at most cap bytes.
ISC_API int32_t isc_scope_options(char* out, int32_t cap)
{
   std::string s;
   for (const sim::ScopeOption& o : sim::ScopeMovieOptions())
      s += std::string(o.name) + "\t" + std::to_string(o.value) + "\t" + o.help + "\n";
   if (out && cap > 0) {
      const size_t n = std::min(s.size(), static_cast<size_t>(cap - 1));
      std::memcpy(out, s.data(), n);
      out[n] = 0;
   }
   return static_cast<int32_t>(s.size());
}

// Renders the movie of `spec` ("k=v k=v ...", see ScopeMovie.h) into out
// (frames * height * width uint16, frame-major). info[0..5] = width, height,
// frames, blinks, WideField dyes, WideField t1/2 in ms (-1 = never bleaches).
// Returns the pixel count the movie needs; renders only if
// capPixels >= that. -1 on a bad spec or a failure (message in errOut).
ISC_API int32_t isc_scope_movie(const char* spec, uint16_t* out, int32_t capPixels, int32_t* info,
                                char* errOut, int32_t errCap)
{
   auto fail = [&](const std::string& e) {
      if (errOut && errCap > 0) {
         const size_t n = std::min(e.size(), static_cast<size_t>(errCap - 1));
         std::memcpy(errOut, e.data(), n);
         errOut[n] = 0;
      }
      return -1;
   };
   if (!spec) return fail("no spec");
   try {
      sim::ScopeSpec s;
      std::string err;
      if (!sim::ParseScopeSpec(spec, s, err)) return fail(err);
      unsigned w, h;
      long n;
      sim::ScopeMovieDims(s, w, h, n);
      const double need = static_cast<double>(w) * h * n;
      if (need > 2.0e9) return fail("movie too large");
      if (info) { info[0] = (int32_t)w; info[1] = (int32_t)h; info[2] = (int32_t)n; info[3] = info[4] = info[5] = 0; }
      if (!out || capPixels < need) return static_cast<int32_t>(need);
      sim::ScopeMovieInfo mi;
      const bool ok = sim::RenderScopeMovie(s, [&](long f, const std::vector<uint16_t>& adu) {
         std::memcpy(out + static_cast<size_t>(f) * w * h, adu.data(), adu.size() * 2);
         return true;
      }, mi, err);
      if (!ok) return fail(err);
      if (info) {
         info[3] = static_cast<int32_t>(mi.blinks);
         info[4] = static_cast<int32_t>(mi.dyes);
         info[5] = std::isfinite(mi.halfTimeSec) ? static_cast<int32_t>(std::min(2.0e9, mi.halfTimeSec * 1000.0)) : -1;
      }
      return static_cast<int32_t>(need);
   } catch (...) {
      return fail("exception");
   }
}

} // extern "C"
