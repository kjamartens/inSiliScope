// SPDX-License-Identifier: BSD-3-Clause
// The GPU for the native cli and server (docs/remote.md "GPU"): "auto" = native WebGPU (wgpu-native and a hardware
// adapter, MakeWebGpuBackend) for the WideField mean-field scenes (WidefieldGpuWebGpu, sim::SetDefaultWidefieldAccelerator)
// and the blink splat + camera noise (SplatGpuHost, sim::SetDefaultBlinkSplatGpu), on one device made on the first use in
// the process; "off" = the CPU. The mode given, else $ISC_GPU, else dflt.
#pragma once

#include "ScopeMovie.h"
#include "SplatGpu.h"
#include "WidefieldGpuWebGpu.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

namespace isc_gpu {

// The process's device: made once (both hosts share it), nullptr (info = why) when there is none.
inline std::shared_ptr<sim::GpuBackend> Device(std::string& info)
{
   static std::mutex mu;
   static bool tried = false;
   static std::shared_ptr<sim::GpuBackend> dev;
   static std::string why;
   std::lock_guard<std::mutex> lock(mu);
   if (!tried)
   {
      tried = true;
      dev = sim::MakeWebGpuBackend(why);
   }
   info = why;
   return dev;
}

// The mode in effect ("auto" or "off"); false (err) for an unknown one. Setting the same mode again keeps the hosts.
inline bool Select(std::string mode, const char* dflt, std::string& err, std::string* resolved = nullptr)
{
   static std::string current;
   if (mode.empty())
   {
      const char* e = std::getenv("ISC_GPU");
      mode = e && *e ? e : dflt;
   }
   if (mode == "0" || mode == "none")
      mode = "off";
   if (mode == "1" || mode == "on")
      mode = "auto";
   if (mode != "auto" && mode != "off")
   {
      err = "--gpu takes auto or off, not " + mode;
      return false;
   }
   if (resolved)
      *resolved = mode;
   if (mode == current)
      return true;
   current = mode;
   if (mode == "off")
   {
      sim::SetDefaultWidefieldAccelerator(nullptr);
      sim::SetDefaultBlinkSplatGpu(nullptr);
   }
   else
   {
      sim::SetDefaultWidefieldAccelerator([](std::string& info) -> std::unique_ptr<sim::WidefieldAccelerator> {
         std::shared_ptr<sim::GpuBackend> dev = Device(info);
         return dev ? sim::WidefieldGpuWebGpu::Create(dev, info) : nullptr;
      });
      sim::SetDefaultBlinkSplatGpu([](std::string& info) -> std::unique_ptr<sim::SplatGpuHost> {
         std::shared_ptr<sim::GpuBackend> dev = Device(info);
         return dev ? sim::MakeSplatGpuHost(dev, info) : nullptr;
      });
   }
   return true;
}

// A GPU and both hosts' self-checks, here and now (the server's probe, in a child of its own); info = the adapter or
// why none.
inline bool Probe(std::string& info)
{
   std::shared_ptr<sim::GpuBackend> dev = sim::MakeWebGpuBackend(info);
   if (!dev)
      return false;
   if (!sim::WidefieldGpuWebGpu::Create(dev, info))
      return false;
   return sim::MakeSplatGpuHost(dev, info) != nullptr;
}

} // namespace isc_gpu
