///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldGpuWebGpu.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The WideField GPU host on native WebGPU (the Linux server's
//                and cli's, docs/remote.md "GPU"): WidefieldGpuHost on a
//                backend that runs WidefieldGpu.wgsl itself -- the viewer's
//                kernels, the same source -- through wgpu-native (on Vulkan).
//
//                The library is loaded at run time (dlopen; no link-time
//                dependency): $ISC_WGPU_LIB, else lib/libwgpu_native.so beside
//                the program's bin/ folder (the release tarball's), else the
//                system's search. No library, no adapter, a software adapter
//                (lavapipe; ISC_GPU_ALLOW_CPU=1 takes it, for tests) or a
//                failed self-check: Create() returns nullptr and says why, and
//                the CPU renders. POSIX only; elsewhere Create() says so.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "WidefieldGpuHost.h"

#include <memory>
#include <string>

namespace sim {

class WidefieldGpuWebGpu : public WidefieldGpuHost
{
public:
   // nullptr (info = why) if the library, an adapter or a device is missing, a kernel does not compile, or the
   // self-check fails; info = the adapter and backend otherwise.
   static std::unique_ptr<WidefieldGpuWebGpu> Create(std::string& info);

private:
   explicit WidefieldGpuWebGpu(std::unique_ptr<GpuBackend> backend) : WidefieldGpuHost(std::move(backend)) {}
};

} // namespace sim
