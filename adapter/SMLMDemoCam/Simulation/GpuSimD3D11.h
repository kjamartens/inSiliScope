///////////////////////////////////////////////////////////////////////////////
// FILE:          GpuSimD3D11.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Optional GPU path for the per-frame work -- vectorial PSF
//                splat + camera noise -- as ONE fused Direct3D 11 compute
//                shader, the analog of webSMLM's WebGPU simulation kernel
//                (build 2026-09-21c). One GPU thread per output pixel gathers
//                that frame's emitters from the kernel's block sums
//                (PsfKernelCache::blockSums, the same numbers the CPU splat
//                reads, with the same SplatSetup() indices/weights), adds the
//                background, and applies the sCMOS or EMCCD noise chain with
//                the same counter-based pcg4d stream as the CPU
//                (SMLMCounterRng.h, mirrored line for line in the HLSL). The
//                CPU and GPU therefore produce the same frame up to float32
//                (GPU) vs float64 (CPU) transcendental rounding -- a pixel
//                whose Poisson draw sits within ~1e-7 of a count boundary may
//                land one ADU apart.
//
//                Direct3D 11 because it ships with every Windows install (no
//                SDK or runtime to add) and runs on any vendor's GPU,
//                integrated included. The HLSL is compiled at runtime with
//                D3DCompile (d3dcompiler_47.dll, part of Windows 10+). A
//                software (WARP/"Microsoft Basic Render Driver") adapter is
//                rejected -- it would be slower than the CPU path.
//
//                One instance per thread: the immediate context is not
//                thread-safe, so the stack worker and the live loop each
//                create their own.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "PsfGeneratorBridge.h"
#include "SMLMNoise.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

// One emitter of one frame, as the GPU reads it: SplatSetup() output plus
// its total photons for the frame (overlap, brightness and illumination
// already folded in) and its z plane. 64 bytes, matching the HLSL struct.
struct GpuSplatEmitter
{
   int32_t x0, y0, bx, by;
   int32_t plane, nTaps;
   float photons, pad;
   float wx[4];
   float wy[4];
};

class GpuSimulator
{
public:
   // nullptr if no usable hardware D3D11 device / shader compile failed;
   // outInfo names the adapter on success, the reason on failure.
   static std::unique_ptr<GpuSimulator> Create(std::string& outInfo);
   ~GpuSimulator();

   // Uploads cache.blockSums (all planes). Must be called again after the
   // cache changes.
   bool SetKernel(const PsfKernelCache& cache, std::string& outError);

   // Per-pixel static inputs for a width x height frame: offset (ADU), gain
   // (photons/ADU), read noise (e-) -- each either width*height long or
   // empty (then the scalar from cam is used) -- and the background in
   // photons/pixel/frame BEFORE the per-frame fade (structured map x
   // illumination, or the flat value x illumination; empty = flatBg).
   bool SetStatic(unsigned width, unsigned height, const std::vector<float>& offset, const std::vector<float>& gain,
                  const std::vector<float>& readNoise, const std::vector<float>& background, double flatBg,
                  const CameraNoiseParams& cam, std::string& outError);

   // Renders + noises one frame into out (width*height, row-major).
   bool RenderFrame(const std::vector<GpuSplatEmitter>& emitters, const CameraNoiseParams& cam,
                    double backgroundScale, uint32_t noiseSeed, uint32_t frame, std::vector<uint16_t>& out,
                    std::string& outError);

   // Same for a batch of frames in ONE dispatch and ONE read-back -- the
   // per-frame GPU round trip is what dominates otherwise. emitters[k],
   // frames[k], backgroundScales[k] describe batch frame k; outs[k] receives
   // it. Batches are split internally to bound GPU memory.
   bool RenderFrames(const std::vector<std::vector<GpuSplatEmitter>>& emitters, const std::vector<uint32_t>& frames,
                     const std::vector<double>& backgroundScales, const CameraNoiseParams& cam, uint32_t noiseSeed,
                     const std::vector<std::vector<uint16_t>*>& outs, std::string& outError);

   // How many frames of this size one RenderFrames dispatch handles.
   size_t MaxBatchFrames() const;

private:
   struct Impl;
   explicit GpuSimulator(std::unique_ptr<Impl> impl);
   std::unique_ptr<Impl> impl_;
};

} // namespace sim
