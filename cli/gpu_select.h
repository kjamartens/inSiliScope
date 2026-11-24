// SPDX-License-Identifier: BSD-3-Clause
// The GPU for the native cli and server (docs/remote.md "GPU"): "auto" = the native WebGPU host
// (WidefieldGpuWebGpu) for the WideField mean-field scenes when wgpu-native and a hardware adapter are there, made on
// its first use (sim::SetDefaultWidefieldAccelerator); "off" = the CPU. The mode given, else $ISC_GPU, else dflt.
#pragma once

#include "ScopeMovie.h"
#include "WidefieldGpuWebGpu.h"

#include <cstdlib>
#include <string>

namespace isc_gpu {

// The mode in effect ("auto" or "off"); false (err) for an unknown one. Setting the same mode again keeps the host.
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
      sim::SetDefaultWidefieldAccelerator(nullptr);
   else
      sim::SetDefaultWidefieldAccelerator([](std::string& info) -> std::unique_ptr<sim::WidefieldAccelerator> {
         return sim::WidefieldGpuWebGpu::Create(info);
      });
   return true;
}

} // namespace isc_gpu
