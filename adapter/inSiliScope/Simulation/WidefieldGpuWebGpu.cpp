///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldGpuWebGpu.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See WidefieldGpuWebGpu.h: the native WebGPU backend of
//                WidefieldGpuHost (wgpu-native, loaded with dlopen). As the
//                viewer's web/wf_gpu.js: one module per kernel (the "common"
//                section and the kernel's), automatic bind group layouts, a
//                uniform block per dispatch.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "WidefieldGpuWebGpu.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include <webgpu/wgpu.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace sim {

namespace {

#include "WidefieldGpuWgsl.inc"   // kWfGpuWgsl: WidefieldGpu.wgsl (cli/CMakeLists.txt writes it)

#define ISC_WGPU_FUNCS(X)                                                                                              \
   X(wgpuCreateInstance)                                                                                               \
   X(wgpuInstanceRequestAdapter)                                                                                       \
   X(wgpuInstanceProcessEvents)                                                                                        \
   X(wgpuInstanceRelease)                                                                                              \
   X(wgpuAdapterRequestDevice)                                                                                         \
   X(wgpuAdapterGetLimits)                                                                                             \
   X(wgpuAdapterGetInfo)                                                                                               \
   X(wgpuAdapterInfoFreeMembers)                                                                                       \
   X(wgpuAdapterRelease)                                                                                               \
   X(wgpuDeviceGetQueue)                                                                                               \
   X(wgpuDeviceCreateBuffer)                                                                                           \
   X(wgpuDeviceCreateShaderModule)                                                                                     \
   X(wgpuDeviceCreateComputePipeline)                                                                                  \
   X(wgpuDeviceCreateBindGroup)                                                                                        \
   X(wgpuDeviceCreateCommandEncoder)                                                                                   \
   X(wgpuDeviceRelease)                                                                                                \
   X(wgpuDevicePoll)                                                                                                   \
   X(wgpuDeviceGetLimits)                                                                                              \
   X(wgpuComputePipelineGetBindGroupLayout)                                                                            \
   X(wgpuComputePipelineRelease)                                                                                       \
   X(wgpuBindGroupLayoutRelease)                                                                                       \
   X(wgpuBindGroupRelease)                                                                                             \
   X(wgpuShaderModuleRelease)                                                                                          \
   X(wgpuBufferRelease)                                                                                                \
   X(wgpuBufferMapAsync)                                                                                               \
   X(wgpuBufferGetConstMappedRange)                                                                                    \
   X(wgpuBufferGetMappedRange)                                                                                         \
   X(wgpuBufferUnmap)                                                                                                  \
   X(wgpuQueueWriteBuffer)                                                                                             \
   X(wgpuQueueSubmit)                                                                                                  \
   X(wgpuQueueRelease)                                                                                                 \
   X(wgpuCommandEncoderBeginComputePass)                                                                               \
   X(wgpuCommandEncoderCopyBufferToBuffer)                                                                             \
   X(wgpuCommandEncoderFinish)                                                                                         \
   X(wgpuCommandEncoderRelease)                                                                                        \
   X(wgpuCommandBufferRelease)                                                                                         \
   X(wgpuComputePassEncoderSetPipeline)                                                                                \
   X(wgpuComputePassEncoderSetBindGroup)                                                                               \
   X(wgpuComputePassEncoderDispatchWorkgroups)                                                                         \
   X(wgpuComputePassEncoderEnd)                                                                                        \
   X(wgpuComputePassEncoderRelease)

struct Api
{
#define ISC_WGPU_PTR(f) decltype(&::f) f = nullptr;
   ISC_WGPU_FUNCS(ISC_WGPU_PTR)
#undef ISC_WGPU_PTR
   void* lib = nullptr;
};

// The library: $ISC_WGPU_LIB, <bin>/../lib/libwgpu_native.so, the system's search; once per process (never closed).
const Api* LoadApi(std::string& err)
{
   static Api api;
   static std::string why;
   static bool tried = false;
   if (tried)
   {
      err = why;
      return api.lib ? &api : nullptr;
   }
   tried = true;
   std::vector<std::string> paths;
   if (const char* e = std::getenv("ISC_WGPU_LIB"))
      if (*e)
         paths.push_back(e);
   char self[PATH_MAX];
   const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
   if (n > 0)
   {
      std::string dir(self, static_cast<size_t>(n));
      dir = dir.substr(0, dir.find_last_of('/'));
      paths.push_back(dir + "/../lib/libwgpu_native.so");
      paths.push_back(dir + "/libwgpu_native.so");
   }
   paths.push_back("libwgpu_native.so");
   std::string tries;
   for (const std::string& p : paths)
   {
      api.lib = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (api.lib)
         break;
      const char* d = dlerror();
      tries += (tries.empty() ? "" : "; ") + std::string(d ? d : p.c_str());
   }
   if (!api.lib)
   {
      why = "no wgpu-native library (" + tries + ")";
      err = why;
      return nullptr;
   }
#define ISC_WGPU_SYM(f)                                                                                                \
   api.f = reinterpret_cast<decltype(api.f)>(dlsym(api.lib, #f));                                                      \
   if (!api.f)                                                                                                         \
   {                                                                                                                   \
      why = std::string("wgpu-native lacks ") + #f;                                                                    \
      api.lib = nullptr;                                                                                               \
      err = why;                                                                                                       \
      return nullptr;                                                                                                  \
   }
   ISC_WGPU_FUNCS(ISC_WGPU_SYM)
#undef ISC_WGPU_SYM
   return &api;
}

WGPUStringView Sv(const char* s)
{
   return WGPUStringView{s, std::strlen(s)};
}

std::string Str(WGPUStringView v)
{
   if (!v.data)
      return {};
   return v.length == WGPU_STRLEN ? std::string(v.data) : std::string(v.data, v.length);
}

struct WgpuBuf : GpuBackend::Buffer
{
   const Api* api = nullptr;
   WGPUBuffer b = nullptr;
   ~WgpuBuf() override
   {
      if (b)
         api->wgpuBufferRelease(b);
   }
};

class WebGpuBackend : public GpuBackend
{
public:
   const Api* api = nullptr;
   WGPUInstance instance = nullptr;
   WGPUAdapter adapter = nullptr;
   WGPUDevice device = nullptr;
   WGPUQueue queue = nullptr;
   WGPULimits limits = WGPU_LIMITS_INIT;
   struct Kernel
   {
      WGPUComputePipeline pipe = nullptr;
      WGPUBindGroupLayout layout = nullptr;
   };
   std::map<std::string, Kernel> kernels;
   WGPUCommandEncoder enc = nullptr;
   std::vector<WGPUBindGroup> groups;   // (released after their submit)
   std::vector<BufPtr> keep;            // the dispatches' uniform blocks, until their submit
   std::shared_ptr<WgpuBuf> staging;
   std::string error;                   // the device's errors since the last check

   ~WebGpuBackend() override
   {
      Submit();
      for (auto& kv : kernels)
      {
         api->wgpuBindGroupLayoutRelease(kv.second.layout);
         api->wgpuComputePipelineRelease(kv.second.pipe);
      }
      staging.reset();
      if (queue)
         api->wgpuQueueRelease(queue);
      if (device)
         api->wgpuDeviceRelease(device);
      if (adapter)
         api->wgpuAdapterRelease(adapter);
      if (instance)
         api->wgpuInstanceRelease(instance);
   }

   static void OnError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void* self, void*)
   {
      std::string& e = static_cast<WebGpuBackend*>(self)->error;
      if (e.empty())
         e = (type == WGPUErrorType_OutOfMemory ? "out of GPU memory: " : "WebGPU error: ") + Str(message);
   }

   bool Failed(std::string& err)
   {
      if (error.empty())
         return false;
      err = error;
      error.clear();
      return true;
   }

   size_t MaxBinding() const override
   {
      return static_cast<size_t>(std::min<uint64_t>(limits.maxStorageBufferBindingSize, limits.maxBufferSize));
   }

   std::shared_ptr<WgpuBuf> MakeRaw(size_t bytes, WGPUBufferUsage usage, const void* data, size_t want,
                                    std::string& err)
   {
      WGPUBufferDescriptor d = {};
      d.usage = usage;
      d.size = bytes;
      d.mappedAtCreation = data ? 1 : 0;
      auto out = std::make_shared<WgpuBuf>();
      out->api = api;
      out->b = api->wgpuDeviceCreateBuffer(device, &d);
      if (!out->b || Failed(err))
      {
         if (err.empty())
            err = "CreateBuffer(" + std::to_string(bytes) + " bytes) failed";
         return nullptr;
      }
      if (data)
      {
         void* p = api->wgpuBufferGetMappedRange(out->b, 0, bytes);
         if (!p)
         {
            err = "buffer not mapped at creation";
            return nullptr;
         }
         std::memcpy(p, data, want);
         if (bytes > want)
            std::memset(static_cast<char*>(p) + want, 0, bytes - want);
         api->wgpuBufferUnmap(out->b);
      }
      out->bytes = bytes;
      return out;
   }

   BufPtr Make(size_t bytes, const void* data, std::string& err) override
   {
      const size_t want = bytes;
      bytes = std::max<size_t>(16, (bytes + 3) / 4 * 4);
      if (bytes > MaxBinding())
      {
         err = "buffer of " + std::to_string(bytes) + " bytes past the device's binding limit (" +
               std::to_string(MaxBinding()) + ")";
         return nullptr;
      }
      return MakeRaw(bytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst, data, want,
                     err);
   }

   WGPUCommandEncoder Encoder()
   {
      if (!enc)
         enc = api->wgpuDeviceCreateCommandEncoder(device, nullptr);
      return enc;
   }

   // The work recorded so far, to the queue.
   void Submit()
   {
      if (!enc)
         return;
      WGPUCommandBuffer cmd = api->wgpuCommandEncoderFinish(enc, nullptr);
      api->wgpuCommandEncoderRelease(enc);
      enc = nullptr;
      if (cmd)
      {
         api->wgpuQueueSubmit(queue, 1, &cmd);
         api->wgpuCommandBufferRelease(cmd);
      }
      for (WGPUBindGroup g : groups)
         api->wgpuBindGroupRelease(g);
      groups.clear();
      keep.clear();
   }

   bool Write(Buffer& b, size_t offset, const void* data, size_t bytes, std::string& err) override
   {
      if (bytes == 0)
         return true;
      Submit();   // (a queue write lands before work not yet submitted)
      std::vector<uint8_t> pad;
      if (bytes % 4)
      {
         pad.assign((bytes + 3) / 4 * 4, 0);
         std::memcpy(pad.data(), data, bytes);
         data = pad.data();
         bytes = pad.size();
      }
      api->wgpuQueueWriteBuffer(queue, static_cast<WgpuBuf&>(b).b, offset, data, bytes);
      return !Failed(err);
   }

   bool Copy(Buffer& dst, Buffer& src, size_t bytes, std::string& err) override
   {
      api->wgpuCommandEncoderCopyBufferToBuffer(Encoder(), static_cast<WgpuBuf&>(src).b, 0,
                                                static_cast<WgpuBuf&>(dst).b, 0, (bytes + 3) / 4 * 4);
      return !Failed(err);
   }

   bool Dispatch(const char* name, const uint32_t* params, size_t nParams, Buffer* const* bufs, size_t nBufs,
                 unsigned x, unsigned y, unsigned z, std::string& err) override
   {
      auto it = kernels.find(name);
      if (it == kernels.end())
      {
         err = std::string("no kernel ") + name;
         return false;
      }
      uint32_t p[16] = {};
      std::memcpy(p, params, std::min<size_t>(nParams, 16) * 4);
      auto ub = MakeRaw(sizeof p, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, p, sizeof p, err);
      if (!ub)
         return false;
      std::vector<WGPUBindGroupEntry> e(nBufs + 1);
      for (size_t i = 0; i <= nBufs; ++i)
      {
         e[i] = {};
         e[i].binding = static_cast<uint32_t>(i);
         e[i].buffer = i == 0 ? ub->b : static_cast<WgpuBuf&>(*bufs[i - 1]).b;
         e[i].offset = 0;
         e[i].size = WGPU_WHOLE_SIZE;
      }
      WGPUBindGroupDescriptor bd = {};
      bd.layout = it->second.layout;
      bd.entryCount = e.size();
      bd.entries = e.data();
      WGPUBindGroup g = api->wgpuDeviceCreateBindGroup(device, &bd);
      if (!g || Failed(err))
      {
         if (err.empty())
            err = std::string("bind group for ") + name;
         err = std::string(name) + ": " + err;
         if (g)
            api->wgpuBindGroupRelease(g);
         return false;
      }
      groups.push_back(g);
      keep.push_back(ub);
      WGPUComputePassEncoder pass = api->wgpuCommandEncoderBeginComputePass(Encoder(), nullptr);
      api->wgpuComputePassEncoderSetPipeline(pass, it->second.pipe);
      api->wgpuComputePassEncoderSetBindGroup(pass, 0, g, 0, nullptr);
      api->wgpuComputePassEncoderDispatchWorkgroups(pass, x, y, z);
      api->wgpuComputePassEncoderEnd(pass);
      api->wgpuComputePassEncoderRelease(pass);
      if (groups.size() >= 256)   // (a long job's uniform blocks and groups go with their submit)
         Submit();
      if (Failed(err))
      {
         err = std::string(name) + ": " + err;
         return false;
      }
      return true;
   }

   bool ReadBack(Buffer& src, size_t bytes, void* dst, std::string& err) override
   {
      const size_t rounded = std::max<size_t>(16, (bytes + 3) / 4 * 4);
      if (!staging || staging->bytes < rounded)
      {
         staging.reset();
         staging = MakeRaw(rounded, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, nullptr, 0, err);
         if (!staging)
            return false;
      }
      api->wgpuCommandEncoderCopyBufferToBuffer(Encoder(), static_cast<WgpuBuf&>(src).b, 0, staging->b, 0,
                                                (bytes + 3) / 4 * 4);
      Submit();
      struct Done
      {
         bool done = false;
         WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Success;
         std::string message;
      } done;
      WGPUBufferMapCallbackInfo cb = {};
      cb.mode = WGPUCallbackMode_AllowProcessEvents;
      cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* u, void*) {
         Done& d = *static_cast<Done*>(u);
         d.done = true;
         d.status = status;
         d.message = Str(message);
      };
      cb.userdata1 = &done;
      api->wgpuBufferMapAsync(staging->b, WGPUMapMode_Read, 0, (bytes + 3) / 4 * 4, cb);
      while (!done.done)
      {
         api->wgpuDevicePoll(device, 1, nullptr);
         api->wgpuInstanceProcessEvents(instance);
         if (!error.empty())
            break;
      }
      if (Failed(err))
         return false;
      if (done.status != WGPUMapAsyncStatus_Success)
      {
         err = "read back: " + done.message;
         return false;
      }
      const void* p = api->wgpuBufferGetConstMappedRange(staging->b, 0, (bytes + 3) / 4 * 4);
      if (!p)
      {
         err = "read back: no mapped range";
         api->wgpuBufferUnmap(staging->b);
         return false;
      }
      std::memcpy(dst, p, bytes);
      api->wgpuBufferUnmap(staging->b);
      return true;
   }

   // Every "//// kernel <name>" section of wgsl, with its "//// common" section prepended, as its own module.
   bool AddKernels(const std::string& text, std::string& err)
   {
      std::string src;
      src.reserve(text.size());
      for (char c : text)
         if (c != '\r')
            src += c;
      std::string common;
      std::vector<std::pair<std::string, std::string>> ks;
      size_t at = src.compare(0, 5, "//// ") == 0 ? 0 : src.find("\n//// ");
      while (at != std::string::npos)
      {
         const size_t head = src[at] == '\n' ? at + 6 : at + 5;
         const size_t nl = src.find('\n', head);
         const size_t next = nl == std::string::npos ? std::string::npos : src.find("\n//// ", nl);
         std::string name = src.substr(head, nl - head);
         while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
         const std::string body =
            nl == std::string::npos ? std::string() : src.substr(nl + 1, next == std::string::npos ? std::string::npos : next + 1 - (nl + 1));
         if (name == "common")
            common = body;
         else if (name.compare(0, 7, "kernel ") == 0)
            ks.emplace_back(name.substr(7), body);
         at = next;
      }
      for (const auto& k : ks)
      {
         const std::string code = common + "\n" + k.second;
         WGPUShaderSourceWGSL wgsl = {};
         wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
         wgsl.code = WGPUStringView{code.data(), code.size()};
         WGPUShaderModuleDescriptor md = {};
         md.nextInChain = &wgsl.chain;
         md.label = Sv(k.first.c_str());
         WGPUShaderModule mod = api->wgpuDeviceCreateShaderModule(device, &md);
         if (!mod || Failed(err))
         {
            err = "WGSL of " + k.first + ": " + (err.empty() ? std::string("no module") : err);
            if (mod)
               api->wgpuShaderModuleRelease(mod);
            return false;
         }
         WGPUComputePipelineDescriptor pd = {};
         pd.layout = nullptr;   // (automatic)
         pd.compute.module = mod;
         pd.compute.entryPoint = Sv(k.first.c_str());
         Kernel kern;
         kern.pipe = api->wgpuDeviceCreateComputePipeline(device, &pd);
         api->wgpuShaderModuleRelease(mod);
         if (!kern.pipe || Failed(err))
         {
            err = "pipeline " + k.first + ": " + (err.empty() ? std::string("not made") : err);
            return false;
         }
         kern.layout = api->wgpuComputePipelineGetBindGroupLayout(kern.pipe, 0);
         kernels[k.first] = kern;
      }
      if (ks.empty())
      {
         err = "no kernels in the WGSL";
         return false;
      }
      return true;
   }
};

const char* BackendName(WGPUBackendType t)
{
   switch (t)
   {
   case WGPUBackendType_Vulkan: return "Vulkan";
   case WGPUBackendType_Metal: return "Metal";
   case WGPUBackendType_D3D12: return "Direct3D 12";
   case WGPUBackendType_D3D11: return "Direct3D 11";
   case WGPUBackendType_OpenGL: return "OpenGL";
   case WGPUBackendType_OpenGLES: return "OpenGL ES";
   default: return "WebGPU";
   }
}

} // namespace

std::unique_ptr<WidefieldGpuWebGpu> WidefieldGpuWebGpu::Create(std::string& info)
{
   const Api* api = LoadApi(info);
   if (!api)
      return nullptr;
   auto be = std::make_unique<WebGpuBackend>();
   be->api = api;
   be->instance = api->wgpuCreateInstance(nullptr);
   if (!be->instance)
   {
      info = "wgpu-native: no instance";
      return nullptr;
   }
   struct Got
   {
      bool done = false;
      void* handle = nullptr;
      std::string message;
   } ga, gd;
   WGPURequestAdapterOptions ao = {};
   ao.powerPreference = WGPUPowerPreference_HighPerformance;
   WGPURequestAdapterCallbackInfo ac = {};
   ac.mode = WGPUCallbackMode_AllowProcessEvents;
   ac.callback = [](WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView message, void* u, void*) {
      Got& g = *static_cast<Got*>(u);
      g.done = true;
      g.handle = status == WGPURequestAdapterStatus_Success ? a : nullptr;
      g.message = Str(message);
   };
   ac.userdata1 = &ga;
   api->wgpuInstanceRequestAdapter(be->instance, &ao, ac);
   for (int i = 0; i < 100000 && !ga.done; ++i)
      api->wgpuInstanceProcessEvents(be->instance);
   be->adapter = static_cast<WGPUAdapter>(ga.handle);
   if (!be->adapter)
   {
      info = "no GPU adapter" + (ga.message.empty() ? std::string() : " (" + ga.message + ")");
      return nullptr;
   }
   WGPUAdapterInfo ai = {};
   api->wgpuAdapterGetInfo(be->adapter, &ai);
   std::string name = Str(ai.device);
   if (name.empty())
      name = Str(ai.description);
   if (name.empty())
      name = "GPU";
   name += std::string(" (") + BackendName(ai.backendType) + ")";
   const bool cpu = ai.adapterType == WGPUAdapterType_CPU;
   api->wgpuAdapterInfoFreeMembers(ai);
   const char* allowCpu = std::getenv("ISC_GPU_ALLOW_CPU");
   if (cpu && !(allowCpu && std::strcmp(allowCpu, "1") == 0))
   {
      info = "only a software adapter is available (" + name + ")";
      return nullptr;
   }
   WGPULimits al = WGPU_LIMITS_INIT;
   api->wgpuAdapterGetLimits(be->adapter, &al);
   WGPULimits want = WGPU_LIMITS_INIT;
   want.maxStorageBufferBindingSize = al.maxStorageBufferBindingSize;
   want.maxBufferSize = al.maxBufferSize;
   WGPUDeviceDescriptor dd = {};
   dd.requiredLimits = &want;
   dd.uncapturedErrorCallbackInfo.callback = &WebGpuBackend::OnError;
   dd.uncapturedErrorCallbackInfo.userdata1 = be.get();
   WGPURequestDeviceCallbackInfo dc = {};
   dc.mode = WGPUCallbackMode_AllowProcessEvents;
   dc.callback = [](WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView message, void* u, void*) {
      Got& g = *static_cast<Got*>(u);
      g.done = true;
      g.handle = status == WGPURequestDeviceStatus_Success ? d : nullptr;
      g.message = Str(message);
   };
   dc.userdata1 = &gd;
   api->wgpuAdapterRequestDevice(be->adapter, &dd, dc);
   for (int i = 0; i < 100000 && !gd.done; ++i)
      api->wgpuInstanceProcessEvents(be->instance);
   be->device = static_cast<WGPUDevice>(gd.handle);
   if (!be->device)
   {
      info = name + ": no device" + (gd.message.empty() ? std::string() : " (" + gd.message + ")");
      return nullptr;
   }
   be->limits = WGPU_LIMITS_INIT;
   api->wgpuDeviceGetLimits(be->device, &be->limits);
   be->queue = api->wgpuDeviceGetQueue(be->device);
   std::string err;
   if (!be->AddKernels(kWfGpuWgsl, err))
   {
      info = name + ": " + err;
      return nullptr;
   }
   std::unique_ptr<WidefieldGpuWebGpu> gpu(new WidefieldGpuWebGpu(std::move(be)));
   if (!gpu->SelfCheck(err))
   {
      info = name + " failed the WideField self-check (" + err + ")";
      return nullptr;
   }
   info = name;
   return gpu;
}

} // namespace sim

#else

namespace sim {

std::unique_ptr<WidefieldGpuWebGpu> WidefieldGpuWebGpu::Create(std::string& info)
{
   info = "the native WebGPU path is for Linux servers";
   return nullptr;
}

} // namespace sim

#endif
