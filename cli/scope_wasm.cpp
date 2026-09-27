// WASM exports of the scope movie (sim::RenderScopeMovie) for the viewer:
// the same render code as the inSiliScope camera and insiliscope_cli, in
// the viewer's module next to the core's C ABI.
#include "ScopeMovie.h"
#include "WidefieldRender.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace {

// A WideField movie whose images come from the viewer's WebGPU
// (web/wf_gpu.js): begin, job (read by the viewer through the getters),
// set_images (or cpu_images), movie, end.
struct WfSession
{
   sim::WidefieldMovie movie;
   sim::ScopeSpec spec;
   sim::WidefieldGpuJob job;
   std::vector<std::vector<int32_t>> deps;
   std::vector<float> cpu;
};
std::map<int32_t, std::unique_ptr<WfSession>> g_wf;
int32_t g_wfNext = 1;

WfSession* Wf(int32_t h)
{
   auto it = g_wf.find(h);
   return it == g_wf.end() ? nullptr : it->second.get();
}

int32_t Fail(char* errOut, int32_t errCap, const std::string& e)
{
   if (errOut && errCap > 0) {
      const size_t n = std::min(e.size(), static_cast<size_t>(errCap - 1));
      std::memcpy(errOut, e.data(), n);
      errOut[n] = 0;
   }
   return -1;
}

int32_t Bits(float f)
{
   int32_t b;
   std::memcpy(&b, &f, 4);
   return b;
}

} // namespace

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

// WideField movie in steps (GPU mode: power-of-two FFTs, images deferred).
// Returns a handle, or -1 (message in errOut).
ISC_API int32_t isc_wf_begin(const char* spec, char* errOut, int32_t errCap)
{
   try {
      std::unique_ptr<WfSession> w(new WfSession);
      std::string err;
      if (!spec || !sim::ParseScopeSpec(spec, w->spec, err)) return Fail(errOut, errCap, err.empty() ? "no spec" : err);
      if (!w->movie.Begin(w->spec, true, err)) return Fail(errOut, errCap, err);
      const int32_t h = g_wfNext++;
      g_wf[h] = std::move(w);
      return h;
   } catch (...) {
      return Fail(errOut, errCap, "exception");
   }
}

// The GPU job of the movie's focus. info[0..13] = NX, NY, nx, ny, fovX0,
// fovY0, cw, ch, kernels, planes, channels, has persistent, dyes, geometry;
// frac[0..1] = sub-cell shift. 0, or -1 if it cannot run on the GPU.
ISC_API int32_t isc_wf_job(int32_t h, int32_t* info, double* frac)
{
   WfSession* w = Wf(h);
   if (!w || !w->movie.Scene().MakeGpuJob(w->job)) return -1;
   const sim::WidefieldGpuJob& j = w->job;
   w->deps.clear();
   for (const auto& ch : j.channels) {
      std::vector<int32_t> d;
      for (const auto& e : ch) {
         d.push_back(static_cast<int32_t>(e.key & 0xffffffffu));
         d.push_back(static_cast<int32_t>(e.key >> 32));
         d.push_back(e.p0);
         d.push_back(e.p1);
         d.push_back(e.two ? 1 : 0);
         d.push_back(Bits(e.w0));
         d.push_back(Bits(e.w1));
      }
      w->deps.push_back(std::move(d));
   }
   if (info) {
      const int32_t v[14] = {(int32_t)j.NX, (int32_t)j.NY, (int32_t)j.nx, (int32_t)j.ny, (int32_t)j.fovX0,
                             (int32_t)j.fovY0, (int32_t)j.cw, (int32_t)j.ch, (int32_t)j.kernels.size(),
                             (int32_t)j.planes.size(), (int32_t)j.channels.size(), j.hasPersistent ? 1 : 0,
                             (int32_t)w->movie.Scene().Dyes(), (int32_t)j.geometry};
      std::memcpy(info, v, sizeof v);
   }
   if (frac) {
      frac[0] = j.fracX;
      frac[1] = j.fracY;
   }
   return 0;
}

// Kernel i's half spectrum ((NX/2+1) x NY complex, interleaved); *p = its plane.
ISC_API const float* isc_wf_kernel(int32_t h, int32_t i, int32_t* p)
{
   WfSession* w = Wf(h);
   if (!w || i < 0 || i >= (int32_t)w->job.kernels.size()) return nullptr;
   if (p) *p = w->job.kernels[i].p;
   return reinterpret_cast<const float*>(w->job.kernels[i].spec->data());
}

// Plane i's cells; meta[0..2] = count, key low, key high; *absSum = sum |values|.
ISC_API const uint32_t* isc_wf_plane(int32_t h, int32_t i, int32_t* meta, float* absSum)
{
   WfSession* w = Wf(h);
   if (!w || i < 0 || i >= (int32_t)w->job.planes.size()) return nullptr;
   const auto& p = w->job.planes[i];
   if (meta) {
      meta[0] = (int32_t)p.cells.size();
      meta[1] = static_cast<int32_t>(p.key & 0xffffffffu);
      meta[2] = static_cast<int32_t>(p.key >> 32);
   }
   if (absSum) *absSum = p.absSum;
   return p.cells.data();
}

ISC_API const float* isc_wf_plane_values(int32_t h, int32_t i)
{
   WfSession* w = Wf(h);
   if (!w || i < 0 || i >= (int32_t)w->job.planes.size()) return nullptr;
   return w->job.planes[i].values.data();
}

// Channel c's deps, 7 int32 each: key low, key high, p0, p1, two, w0 bits,
// w1 bits.
ISC_API const int32_t* isc_wf_channel(int32_t h, int32_t c, int32_t* nDeps)
{
   WfSession* w = Wf(h);
   if (!w || c < 0 || c >= (int32_t)w->deps.size()) return nullptr;
   if (nDeps) *nDeps = (int32_t)(w->deps[c].size() / 7);
   return w->deps[c].data();
}

// The images of every job channel, channels * cw * ch floats.
ISC_API int32_t isc_wf_set_images(int32_t h, const float* data)
{
   WfSession* w = Wf(h);
   if (!w || !data) return -1;
   const size_t n = static_cast<size_t>(w->job.cw) * w->job.ch;
   std::vector<std::vector<float>> imgs(w->job.channels.size());
   for (size_t c = 0; c < imgs.size(); c++) imgs[c].assign(data + c * n, data + (c + 1) * n);
   return w->movie.Scene().SetImages(imgs) ? 0 : -1;
}

// The same images on the CPU (and adopted); copied to out if given.
ISC_API int32_t isc_wf_cpu_images(int32_t h, float* out)
{
   WfSession* w = Wf(h);
   if (!w) return -1;
   w->movie.ComputeCpuImages();
   if (out) {
      const sim::WidefieldImages& im = w->movie.Scene().Images();
      const size_t n = static_cast<size_t>(im.cw) * im.ch;
      size_t c = 0;
      if (!im.persistent.empty()) std::memcpy(out + n * c++, im.persistent.data(), n * 4);
      for (const auto& b : im.bleach) std::memcpy(out + n * c++, b.data(), n * 4);
   }
   return 0;
}

// The frames, as isc_scope_movie.
ISC_API int32_t isc_wf_movie(int32_t h, uint16_t* out, int32_t capPixels, int32_t* info, char* errOut, int32_t errCap)
{
   WfSession* w = Wf(h);
   if (!w) return Fail(errOut, errCap, "no WideField session");
   try {
      unsigned W, H;
      long n;
      sim::ScopeMovieDims(w->spec, W, H, n);
      const double need = static_cast<double>(W) * H * n;
      if (info) { info[0] = (int32_t)W; info[1] = (int32_t)H; info[2] = (int32_t)n; info[3] = info[4] = info[5] = 0; }
      if (!out || capPixels < need) return static_cast<int32_t>(need);
      sim::ScopeMovieInfo mi;
      std::string err;
      const bool ok = w->movie.Render([&](long f, const std::vector<uint16_t>& adu) {
         std::memcpy(out + static_cast<size_t>(f) * W * H, adu.data(), adu.size() * 2);
         return true;
      }, mi, err);
      if (!ok) return Fail(errOut, errCap, err);
      if (info) {
         info[4] = static_cast<int32_t>(mi.dyes);
         info[5] = std::isfinite(mi.halfTimeSec) ? static_cast<int32_t>(std::min(2.0e9, mi.halfTimeSec * 1000.0)) : -1;
      }
      return static_cast<int32_t>(need);
   } catch (...) {
      return Fail(errOut, errCap, "exception");
   }
}

ISC_API void isc_wf_end(int32_t h)
{
   g_wf.erase(h);
}

} // extern "C"
