///////////////////////////////////////////////////////////////////////////////
// FILE:          SplatGpu.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The blink splat + camera noise on a GPU backend (GpuBackend,
//                WidefieldGpuHost.h) running SplatGpu.wgsl: the native cli's and
//                server's (docs/remote.md "GPU"), on the same WebGPU device as
//                the WideField host. The WGSL twin of the adapter's Direct3D 11
//                GpuSimulator (GpuSimD3D11.*): the same emitter records
//                (CollectGpuEmitters), block sums, halo spans and noise stream.
//
//                FluorescenceMovie::Render hands it each frame's splatted
//                blinks and the rest of the frame's light as one extra photon
//                image (the background, the continuous populations, binned
//                blinks: drawn on the CPU as before); the GPU adds the blinks
//                and the noise (a movie without blinks: the noise alone). Frames equal the CPU's up to float32 rounding
//                (a Poisson draw near a count boundary one count apart).
//
//                Kernels stay resident by their planes (PsfKernelCache::Serial,
//                the 4 used last), the camera maps by the caller's key: a live
//                frame uploads only its emitters and its extra image.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "GpuSimD3D11.h"   // GpuSplatEmitter
#include "PsfGeneratorBridge.h"
#include "SMLMNoise.h"
#include "WidefieldGpuHost.h"   // GpuBackend

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

class SplatGpuHost
{
public:
   static constexpr size_t kMaxKernels = 4;
   explicit SplatGpuHost(std::shared_ptr<GpuBackend> backend);
   ~SplatGpuHost();

   // The kernels an emitter names by index (GpuSplatEmitter::plane >> 16), up to kMaxKernels (none: frames of the
   // extra images and the noise alone); diffraction kernels only (no Fft placement). Kernels with the same planes share
   // a buffer; one past the backend's binding limit takes a buffer (and a table entry) per range of its planes.
   bool SetKernels(const std::vector<const PsfKernelCache*>& kernels, std::string& err);
   // A width x height frame's camera maps (each width*height, or empty: cam's scalar) and a window's first pixel on the
   // sensor (the noise is drawn at the sensor's pixels). key != 0 and the same as the last call's: kept as they are.
   bool SetStatic(unsigned width, unsigned height, uint32_t pixelBase, const std::vector<float>& offset,
                  const std::vector<float>& gain, const std::vector<float>& readNoise, const CameraNoiseParams& cam,
                  uint64_t key, std::string& err);
   // Frames k: emitters[k] splatted onto (*extras[k]) (width*height photons; nullptr: none), the noise keyed by (seed,
   // frames[k]); outs[k] receives the ADU. Split into MaxBatchFrames() per dispatch.
   bool RenderFrames(const std::vector<std::vector<GpuSplatEmitter>>& emitters, const std::vector<uint32_t>& frames,
                     const std::vector<const std::vector<float>*>& extras, const CameraNoiseParams& cam,
                     uint32_t seed, const std::vector<std::vector<uint16_t>*>& outs, std::string& err);
   size_t MaxBatchFrames() const;

   // A small kernel's blinks on the GPU against RenderPhotonImage + ApplyNoiseChain (>= 90 % of the pixels equal, the
   // rest within a few ADU on average), so a driver that gets the kernel wrong leaves the CPU in charge.
   bool SelfCheck(std::string& err);
   GpuBackend& Backend() { return *backend_; }

private:
   struct State;
   std::shared_ptr<GpuBackend> backend_;
   std::unique_ptr<State> s_;
};

// A host on backend that passed its self-check; nullptr (info = why) else.
std::unique_ptr<SplatGpuHost> MakeSplatGpuHost(std::shared_ptr<GpuBackend> backend, std::string& info);

} // namespace sim
