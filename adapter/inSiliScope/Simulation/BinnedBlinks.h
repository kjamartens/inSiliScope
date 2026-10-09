///////////////////////////////////////////////////////////////////////////////
// FILE:          BinnedBlinks.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The "binned" blink regime (approximate SMLM): a frame's blink
//                emitters (CollectFrameEmitters: the splat's photons, position
//                and kernel z plane) are deposited on a grid of `upscale`
//                cells per camera pixel, one density per PSF z plane, each
//                plane FFT-convolved with that plane's kernel resampled to the
//                grid (KernelWidefieldPsf), summed in the spectrum, one
//                cropped inverse, max(0, .) per cell, binned to camera pixels.
//                The same events as the splat; only the xy position is snapped
//                to the nearest cell (+-pitch/2). Cost per frame ~ occupied
//                planes x one real FFT, independent of the event count, so it
//                wins over the splat at high blink densities (spec/ALGORITHM.md
//                "Blink render regimes").
//
//                The kernel spectra are shared process-wide (keyed by the
//                kernel, upscale and grid), so live frames reuse them.
//                JS twin: web/prototype/scope/binned_blinks.js.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "PsfGeneratorBridge.h"
#include "SMLMSimulation.h"

#include <memory>
#include <vector>

namespace sim {

struct BinnedKernelSpectra;

class BinnedBlinkRenderer
{
public:
   // kernel: the blink kernel (its halo spans, if any, bound the kernel radius;
   // the cells inside it are the whole kernel's), or null / invalid for the
   // in-focus Gaussian of sigmaPx. upscale: cells per camera pixel (made a
   // divisor of the kernel's oversampling, KernelWidefieldPsf::ValidUpscale).
   bool Setup(const PsfKernelCache* kernel, double sigmaPx, unsigned width, unsigned height, int upscale);
   bool Valid() const { return spectra_ != nullptr; }
   int Upscale() const { return u_; }
   int RadiusCells() const { return R_; }

   // img (width x height) += the emitters' photon image. Emitters whose cell
   // lies more than the kernel radius outside the FOV reach no pixel and are
   // skipped. The FFT lines run on all cores (one live frame), serially inside
   // a ParallelFor worker (frames in parallel); the same result either way.
   void Render(const std::vector<FrameEmitter>& emitters, std::vector<float>& img) const;

private:
   unsigned W_ = 0, H_ = 0;
   int u_ = 1, R_ = 1;
   std::shared_ptr<BinnedKernelSpectra> spectra_;
};

} // namespace sim
