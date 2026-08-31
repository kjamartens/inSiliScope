///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMSimulation.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The core SMLM blinking/PSF/noise model, ported from the
//                webSMLM reference simulator (webSMLM/index.html): a Poisson
//                emitter-arrival process, single exponential ON-lifetime
//                blinking, additive 2D Gaussian PSF rendering, and (via
//                SMLMNoise.h) a realistic camera noise chain.
//
//                This header has no MMDevice includes at all -- it is
//                compilable/testable standalone, independent of Micro-
//                Manager.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "SMLMNoise.h"
#include "SMLMPatterns.h"
#include "PsfGeneratorBridge.h"

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace sim {

struct SimulationParams
{
   double pixelSizeNm = 100.0;          // simulation pixel size, nm
   double emitterDensity = 0.5;         // emitters / um^2 / frame
   double photonsPerBlink = 2000.0;     // photons emitted per full ON frame
   double onLifetimeFrames = 3.0;       // mean exponential ON duration, frames
   double psfSigmaPx = 1.3;             // Gaussian PSF sigma, pixels
   double backgroundPhotons = 20.0;     // additive background, photons/pixel
   double gainPhotonsPerAdu = 1.0;
   double offsetAdu = 100.0;
   double offsetStdAdu = 2.0;           // per-pixel fixed-pattern offset std
   double readNoiseElectrons = 1.5;
   // Stage-drift rate along X, nm/sec; Y drifts at half this rate (fixed
   // diagonal direction, not random) -- see ComputeDriftOffsetPx().
   double driftNmPerSecX = 0.0;
   // Wall-clock duration of one frame, seconds (derived from the camera's
   // current Exposure). Needed alongside driftNmPerSecX to convert an
   // elapsed frame count into an elapsed time for the drift ramp.
   double frameDurationSec = 0.001;
};

// A single blinking event: one emitter turning on at tStart (in frame units)
// and decaying off at tEnd (tEnd - tStart is drawn from an exponential
// distribution with mean = onLifetimeFrames).
struct BlinkEvent
{
   double xUm = 0.0;
   double yUm = 0.0;
   double tStart = 0.0;
   double tEnd = 0.0;
};

// Linear stage-drift offset (pixels) at elapsedSec seconds since the drift
// origin (acquisition start): X moves at driftNmPerSecX, Y at half that
// rate, both starting at (0,0). Direction is fixed (not random) so drift is
// reproducible and trivially resets to zero by resetting elapsedSec.
void ComputeDriftOffsetPx(double elapsedSec, double driftNmPerSecX, double pixelSizeNm,
                           double& outDx, double& outDy);

// Splats a photon-conserving 2D Gaussian PSF additively into img (a
// width*height photon-count buffer), centered at the (sub-pixel) position
// (xPx, yPx).
void RenderGaussianPSF(std::vector<float>& img, unsigned width, unsigned height,
                        double xPx, double yPx, double sigmaPx, double totalPhotons);

// Renders one frame's clean photon-count image (background + all emitters
// whose [tStart,tEnd) overlaps [frameIndex, frameIndex+1)) into img (resized
// to width*height as needed).
//
// psfCache: when non-null and valid, each emitter is rendered by
// downsampling+splatting the cached oversampled vectorial PSF kernel
// (PsfGeneratorBridge.h) instead of the analytic Gaussian -- psfSigmaPx is
// then unused. Defaults to nullptr so every existing call site (Gaussian
// rendering) is unaffected.
void RenderPhotonImage(std::vector<float>& img, unsigned width, unsigned height,
                        const std::vector<BlinkEvent>& events, long frameIndex,
                        double pixelSizeNm, double psfSigmaPx, double photonsPerBlink,
                        double backgroundPhotons,
                        double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache* psfCache = nullptr);

// Owns the active pattern and the emitter blinking process, and provides one
// code path shared by both acquisition modes: GenerateAllEvents for a whole
// precomputed stack up front, AdvanceOneFrame for one live-mode tick.
class EmitterModel
{
public:
   void SetPattern(std::unique_ptr<IPatternGenerator> pattern);
   void Reseed(uint64_t seed);

   // Precomputed mode: builds the complete event list covering frames
   // [0, nFrames), including a lead-in window before frame 0 so early frames
   // aren't empty.
   std::vector<BlinkEvent> GenerateAllEvents(long nFrames, double widthUm, double heightUm,
                                              const SimulationParams& params,
                                              std::mt19937_64& rng) const;

   // Live mode: call once before the first AdvanceOneFrame() after a pattern
   // change or reseed to clear any in-flight events.
   void ResetLive(double widthUm, double heightUm);

   // Live mode: advances the model by one frame, spawning new blink events
   // via the Poisson-arrival process at the *current* params.emitterDensity,
   // retiring events that have fully elapsed, and returning every event
   // (existing + newly spawned) overlapping this frame.
   std::vector<BlinkEvent> AdvanceOneFrame(long frameIndex, double widthUm, double heightUm,
                                            const SimulationParams& params,
                                            std::mt19937_64& rng);

private:
   std::unique_ptr<IPatternGenerator> pattern_;
   std::vector<BlinkEvent> liveActive_;
};

} // namespace sim
