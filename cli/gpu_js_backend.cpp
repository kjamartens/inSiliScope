// SPDX-License-Identifier: BSD-3-Clause
// The viewer module's GPU for the blink splat + camera noise (2026-11-24): a sim::GpuBackend whose calls go to the
// worker's WebGPU device (Module.iscGpu, web/splat_gpu.js), so SplatGpuHost (Simulation/SplatGpu.*) runs
// SplatGpu.wgsl in the browser, with its emitter records, batches and self-check.
//
// The movie stays synchronous (isc_scope_movie, isc_wf_movie, Module.onMovieFrame): WebGPU's mapAsync cannot resolve
// while the module runs, so the read-back goes through an OffscreenCanvas instead -- the frames' words copied into a
// canvas texture, drawn into a 2D canvas, read with getImageData, which waits for the GPU (TensorFlow.js's readSync
// for its WebGPU backend). An opaque canvas keeps a texel's red, green and blue bytes exactly, which carries the low 16
// bits of a word (GpuBackend::ReadBackLow16). The weighed alternatives: a deferred read-back (frames delivered as
// their maps resolve) needs the movie to return to the event loop between batches, which FluorescenceMovie::Render
// does not (a resumable Render, or a Record cut into movies, which changes its frames); JSPI suspends the module
// instead, but a JSPI build does not instantiate where the browser lacks it (Firefox, Safari), and every job of the
// worker could run inside a suspended movie.
//
// isc_gpu_splat(1) installs it as the engine's blink splat (sim::SetDefaultBlinkSplatGpu; the host and its self-check
// are made on the first movie that splats), 0 removes it, -1 asks: 2 a host passed its self-check, -1 it failed (the
// reason on stderr, the console), 1 installed and not tried yet, 0 off. Live frames add their own noise (onPhotons)
// and never use it; nor does a ground-truth run.
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

#include "ScopeMovie.h"
#include "SplatGpu.h"
#include "WidefieldGpuHost.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define ISC_API extern "C" EMSCRIPTEN_KEEPALIVE

// Pointers above 2 GB are negative i32 in JS: >>> 0. Sizes go as doubles.
EM_JS(int, isc_js_gpu_present, (), { return Module.iscGpu && !Module.iscGpu.lost ? 1 : 0; });
EM_JS(double, isc_js_gpu_max_binding, (), { return Module.iscGpu ? Module.iscGpu.maxBinding : 0; });
EM_JS(int, isc_js_gpu_name, (char* out, int cap), {
   return stringToUTF8((Module.iscGpu && Module.iscGpu.name) || 'WebGPU', out >>> 0, cap);
});
EM_JS(int, isc_js_gpu_error, (char* out, int cap), {
   return stringToUTF8((Module.iscGpu && Module.iscGpu.err) || 'no WebGPU device', out >>> 0, cap);
});
EM_JS(int, isc_js_gpu_make, (double bytes, const void* data), {
   const p = data >>> 0;
   return Module.iscGpu ? Module.iscGpu.make(bytes, p ? HEAPU8.subarray(p, p + bytes) : null) : 0;
});
EM_JS(int, isc_js_gpu_write, (int id, double offset, const void* data, double bytes), {
   const p = data >>> 0;
   return Module.iscGpu ? Module.iscGpu.write(id, offset, HEAPU8.subarray(p, p + bytes)) : 0;
});
EM_JS(int, isc_js_gpu_copy, (int dst, int src, double bytes), {
   return Module.iscGpu ? Module.iscGpu.copy(dst, src, bytes) : 0;
});
EM_JS(int, isc_js_gpu_dispatch, (const char* name, const uint32_t* params, const int32_t* ids, int nIds, int x, int y,
                                 int z), {
   const p = (params >>> 0) / 4, b = (ids >>> 0) / 4;
   return Module.iscGpu ? Module.iscGpu.dispatch(UTF8ToString(name >>> 0), HEAPU32.slice(p, p + 16),
                                                 Array.from(HEAP32.subarray(b, b + nIds)), x, y, z) : 0;
});
EM_JS(int, isc_js_gpu_read16, (int id, double words, uint16_t* dst), {
   const p = (dst >>> 0) / 2;
   return Module.iscGpu ? Module.iscGpu.read16(id, words, HEAPU16.subarray(p, p + words)) : 0;
});
EM_JS(void, isc_js_gpu_free, (int id), {
   if (Module.iscGpu) Module.iscGpu.free(id);
});

namespace {

std::string JsText(int (*fn)(char*, int))
{
   char b[512];
   b[0] = 0;
   fn(b, sizeof b);
   return b;
}

class JsGpuBackend : public sim::GpuBackend
{
public:
   struct JsBuf : Buffer
   {
      int id = 0;
      ~JsBuf() override
      {
         if (id)
            isc_js_gpu_free(id);
      }
   };

   JsGpuBackend()
   {
      name = JsText(isc_js_gpu_name);
      maxBinding_ = static_cast<size_t>(isc_js_gpu_max_binding());
   }

   BufPtr Make(size_t bytes, const void* data, std::string& err) override
   {
      bytes = std::max<size_t>(16, (bytes + 3) / 4 * 4);
      if (bytes > MaxBinding())
      {
         err = "a buffer of " + std::to_string(bytes >> 20) + " MB passes the binding limit (" +
               std::to_string(MaxBinding() >> 20) + " MB)";
         return nullptr;
      }
      std::shared_ptr<JsBuf> b(new JsBuf);
      b->id = isc_js_gpu_make(static_cast<double>(bytes), data);
      if (!b->id)
      {
         err = JsText(isc_js_gpu_error);
         return nullptr;
      }
      b->bytes = bytes;
      return b;
   }
   bool Write(Buffer& b, size_t offset, const void* data, size_t bytes, std::string& err) override
   {
      return Ok(isc_js_gpu_write(Id(b), static_cast<double>(offset), data, static_cast<double>(bytes)), err);
   }
   bool Copy(Buffer& dst, Buffer& src, size_t bytes, std::string& err) override
   {
      return Ok(isc_js_gpu_copy(Id(dst), Id(src), static_cast<double>(bytes)), err);
   }
   bool Dispatch(const char* kernel, const uint32_t* params, size_t nParams, Buffer* const* bufs, size_t nBufs,
                 unsigned x, unsigned y, unsigned z, std::string& err) override
   {
      uint32_t p[16] = {};
      std::copy(params, params + std::min<size_t>(nParams, 16), p);
      std::vector<int32_t> ids(nBufs);
      for (size_t i = 0; i < nBufs; ++i)
         ids[i] = Id(*bufs[i]);
      return Ok(isc_js_gpu_dispatch(kernel, p, ids.data(), static_cast<int>(nBufs), static_cast<int>(x),
                                    static_cast<int>(y), static_cast<int>(z)), err);
   }
   bool ReadBack(Buffer&, size_t, void*, std::string& err) override
   {
      err = "the viewer's GPU reads back 16-bit words only (ReadBackLow16)";
      return false;
   }
   bool ReadBackLow16(Buffer& src, size_t words, uint16_t* dst, std::string& err) override
   {
      return Ok(isc_js_gpu_read16(Id(src), static_cast<double>(words), dst), err);
   }
   size_t MaxBinding() const override { return maxBinding_; }

private:
   static int Id(Buffer& b) { return static_cast<JsBuf&>(b).id; }
   static bool Ok(int r, std::string& err)
   {
      if (!r)
         err = JsText(isc_js_gpu_error);
      return r != 0;
   }
   size_t maxBinding_ = 0;
};

int g_state = 0;   // isc_gpu_splat(-1)

} // namespace

ISC_API int32_t isc_gpu_splat(int32_t on)
{
   if (on < 0)
      return g_state;
   if (on == 0)
   {
      if (g_state != 0)
         sim::SetDefaultBlinkSplatGpu(nullptr);
      g_state = 0;
      return 0;
   }
   if (g_state == 0)
   {
      g_state = 1;
      sim::SetDefaultBlinkSplatGpu([](std::string& info) -> std::unique_ptr<sim::SplatGpuHost> {
         if (!isc_js_gpu_present())
         {
            info = "no WebGPU device";
            g_state = -1;
            return nullptr;
         }
         std::unique_ptr<sim::SplatGpuHost> h = sim::MakeSplatGpuHost(std::make_shared<JsGpuBackend>(), info);
         g_state = h ? 2 : -1;
         if (!h)
            std::fprintf(stderr, "the blink splat and noise on the CPU: %s\n", info.c_str());
         return h;
      });
   }
   return g_state;
}
#endif
