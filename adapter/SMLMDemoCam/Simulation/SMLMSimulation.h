///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMSimulation.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The core SMLM blinking/PSF/noise model, ported from the
//                webSMLM reference simulator (webSMLM.html): a Poisson
//                emitter-arrival process, exponential ON-lifetime blinking
//                (single-blink by default, or a three-state ON <-> dark ->
//                bleached multi-blink model with log-normal per-blink
//                brightness), additive PSF rendering, and (via SMLMNoise.h) a
//                realistic camera noise chain.
//
//                This header has no MMDevice includes at all -- it is
//                compilable/testable standalone, independent of Micro-
//                Manager.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "GpuSimD3D11.h"
#include "SMLMBackground.h"
#include "SMLMNoise.h"
#include "SMLMPatterns.h"
#include "PsfGeneratorBridge.h"

#include <cstdint>
#include <functional>
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
   double quantumEfficiency = 0.85;     // incident photons -> detected electrons
   double darkCurrentElectronsPerFrame = 0.0; // thermal dark counts this frame
   double gainPhotonsPerAdu = 1.0;
   double offsetAdu = 100.0;
   double offsetStdAdu = 2.0;           // per-pixel fixed-pattern offset std
   double readNoiseElectrons = 1.5;
   // Pixel-to-pixel relative std of gain/read noise (sCMOS-style per-pixel
   // maps), as a fraction of the nominal gain/readNoiseElectrons above.
   // 0 = every pixel identical (old scalar behavior).
   double pixelGainStdFraction = 0.0;
   double pixelReadNoiseStdFraction = 0.0;
   // Multi-blink photophysics (webSMLM's simulation_blinkBleachProb/
   // simulation_offLifetime/simulation_photCV). After each ON period a
   // molecule bleaches with probability blinkBleachProb, otherwise it goes
   // dark for an exponential time of mean offLifetimeFrames and blinks
   // again -- a geometric number of blinks, mean 1/blinkBleachProb.
   // photonCV > 0 makes each blink's photon rate log-normal with that
   // coefficient of variation, mean preserved. The defaults (1 and 0) select
   // the original single-blink, constant-brightness model, whose rng draw
   // sequence is untouched -- see EmitterModel::GenerateAllEvents.
   double blinkBleachProb = 1.0;
   double offLifetimeFrames = 20.0;
   double photonCV = 0.0;
   // Stage-drift speed, nm/sec, along a direction driftAngleRad drawn once
   // per RandomSeed (see ComputeDriftOffsetPx).
   double driftNmPerSecX = 0.0;
   double driftAngleRad = 0.0;
   // Sensor model beyond the scalars above: EMCCD path switch and its
   // parameters -- see CameraNoiseParams in SMLMNoise.h.
   bool emccd = false;
   double emGain = 300.0;
   double cicElectrons = 0.002;
   int bitDepth = 16;
   // Wall-clock duration of one frame, seconds (derived from the camera's
   // current Exposure). Needed alongside driftNmPerSecX to convert an
   // elapsed frame count into an elapsed time for the drift ramp.
   double frameDurationSec = 0.001;

   CameraNoiseParams Camera() const
   {
      CameraNoiseParams c;
      c.quantumEfficiency = quantumEfficiency;
      c.darkCurrentElectrons = darkCurrentElectronsPerFrame;
      c.gainPhotonsPerAdu = gainPhotonsPerAdu;
      c.readNoiseElectrons = readNoiseElectrons;
      c.emccd = emccd;
      c.emGain = emGain;
      c.cicElectrons = cicElectrons;
      c.bitDepth = bitDepth;
      return c;
   }
};

// A single blinking event: one emitter turning on at tStart (in frame units)
// and decaying off at tEnd (tEnd - tStart is drawn from an exponential
// distribution with mean = onLifetimeFrames).
struct BlinkEvent
{
   double xUm = 0.0;
   double yUm = 0.0;
   double zNm = 0.0; // carried straight through from EmitterSite::zNm
   double tStart = 0.0;
   double tEnd = 0.0;
   // Per-blink photon-rate factor (log-normal, mean 1, when photonCV > 0;
   // exactly 1 otherwise). Multiplies photonsPerBlink at render time.
   double brightness = 1.0;
   // Live mode, multi-blink model only: true while this blink's molecule has
   // not yet decided (at tEnd) whether it bleaches or blinks again -- see
   // EmitterModel::AdvanceOneFrame.
   bool moleculeLive = false;
};

// Linear stage-drift offset (pixels) at elapsedSec seconds since the drift
// origin (acquisition start): speed driftNmPerSec along angleRad, starting
// at (0,0). The angle is drawn once per RandomSeed from its own rng stream
// (DriftAngleForSeed) -- a random direction like webSMLM's, but still
// reproducible, and drift still resets to zero by resetting elapsedSec.
void ComputeDriftOffsetPx(double elapsedSec, double driftNmPerSec, double angleRad, double pixelSizeNm,
                           double& outDx, double& outDy);

// Uniform [0, 2*pi) drift direction for a RandomSeed, drawn from a dedicated
// stream (seed ^ "DRIFTDIR") so it never shifts any other stream.
double DriftAngleForSeed(long seed);

// Optional per-frame inputs to RenderPhotonImage beyond the flat background
// -- all "off" by default, which keeps the original output byte-identical.
struct RenderExtras
{
   // Illumination field (SMLMBackground.h BuildIlluminationField, peak 1),
   // width*height, or nullptr/empty for flat. Multiplies both the background
   // and each emitter's photons (read at the emitter's undrifted site, as
   // webSMLM does).
   const std::vector<float>* illumField = nullptr;
   // Structured background map (photons/pixel/frame, BuildBackgroundMap),
   // width*height, replacing the flat backgroundPhotons when non-empty.
   const std::vector<float>* backgroundMap = nullptr;
   // Background fade factor for this frame (BackgroundFadeScale).
   double backgroundScale = 1.0;
};

// The fixed (per stack / per live config) spatial fields behind a
// RenderExtras: an illumination field and a structured background map, each
// empty when its feature is off.
struct StackShapingFields
{
   std::vector<float> illum;
   std::vector<float> background;

   // Extras for a frame at elapsed time tSec with fade constant decaySec
   // (both seconds; decaySec <= 0 = no fade). The pointers borrow this
   // object's vectors, so it must outlive the returned value's use.
   RenderExtras Extras(double tSec, double decaySec) const
   {
      RenderExtras e;
      e.illumField = illum.empty() ? nullptr : &illum;
      e.backgroundMap = background.empty() ? nullptr : &background;
      e.backgroundScale = BackgroundFadeScale(tSec, decaySec);
      return e;
   }
};

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
//
// globalZOffsetUm: uniform focus offset (micrometers), driven by the
// SMLMDemoZStage device's shared position (Simulation/SharedStageState.h),
// ADDED to each emitter's own BlinkEvent::zNm (nm, converted to um here) to
// pick that emitter's kernel z-plane -- so the stage moves the focal plane
// and the structure's own depth is relative to it, not a single global
// plane shared by every emitter. 0 offset + 0 zNm = center/in-focus plane.
// The plane lookup is nearest-plane only, done PER EMITTER (not once per
// frame the way it used to be, back when every emitter shared one z) --
// deliberately not a two-plane blend: blending two planes' intensities is
// not the same operation as interpolating a PSF's width, so it would buy no
// accuracy while costing meaningfully more (the reference simulator this
// project tracks parity with implemented and then removed exactly this
// blend for that reason -- see docs/vectorial-psf-plan.md).
//
// outZClampedCount/outZTotalCount (optional, default nullptr): accumulated
// (+=, not assigned) counts of vectorial-PSF emitter renders whose total z
// fell outside the cached kernel's own range (clamped to an end plane) vs.
// the total rendered -- callers use this to warn once per stack/config
// rather than per emitter.
void RenderPhotonImage(std::vector<float>& img, unsigned width, unsigned height,
                        const std::vector<BlinkEvent>& events, long frameIndex,
                        double pixelSizeNm, double psfSigmaPx, double photonsPerBlink,
                        double backgroundPhotons,
                        double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache* psfCache = nullptr,
                        double globalZOffsetUm = 0.0,
                        long* outZClampedCount = nullptr,
                        long* outZTotalCount = nullptr,
                        const RenderExtras* extras = nullptr);

// Indices into events of the blinks overlapping each frame [f, f+1), for f in
// [0, nFrames), in event order -- so a frame renders only its own emitters
// (and in the same order as scanning the whole list would), instead of every
// frame scanning every event of the movie.
std::vector<std::vector<uint32_t>> BucketEventsByFrame(const std::vector<BlinkEvent>& events, long nFrames);

// The GPU path's half of RenderPhotonImage: this frame's vectorial-PSF
// emitters as GpuSplatEmitter records (same photons -- overlap x brightness
// x illumination --, same per-emitter z plane, same SplatSetup), appended to
// out. The background is applied on the GPU itself. Not for Fft placement.
void CollectGpuEmitters(const std::vector<BlinkEvent>& events, long frameIndex, unsigned width, unsigned height,
                        double pixelSizeNm, double photonsPerBlink, double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache& cache, double globalZOffsetUm, const RenderExtras* extras,
                        std::vector<GpuSplatEmitter>& out, long* outZClampedCount = nullptr,
                        long* outZTotalCount = nullptr);

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
   // aren't empty. densityScale multiplies params.emitterDensity (used for
   // the out-of-focus population, which runs the same kinetics at
   // Background_OutOfFocusRatio times the density); zOverride, when set,
   // replaces each molecule's site depth with a draw from it.
   std::vector<BlinkEvent> GenerateAllEvents(long nFrames, double widthUm, double heightUm,
                                              const SimulationParams& params,
                                              std::mt19937_64& rng, double densityScale,
                                              const std::function<double(std::mt19937_64&)>& zOverride) const;
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
   // Same, with the densityScale/zOverride of GenerateAllEvents' overload
   // (out-of-focus population -- give it its own EmitterModel instance, since
   // this carries in-flight events across ticks).
   std::vector<BlinkEvent> AdvanceOneFrame(long frameIndex, double widthUm, double heightUm,
                                            const SimulationParams& params, std::mt19937_64& rng,
                                            double densityScale,
                                            const std::function<double(std::mt19937_64&)>& zOverride);

   // Every site the pattern can produce, as a (bounded) sample: the full
   // list for a SiteListPattern, else maxSamples SampleSite draws from rng.
   // For the static out-of-focus haze map.
   std::vector<EmitterSite> SampleSitesForHaze(double widthUm, double heightUm, size_t maxSamples,
                                               std::mt19937_64& rng) const;

private:
   std::unique_ptr<IPatternGenerator> pattern_;
   std::vector<BlinkEvent> liveActive_;
};

} // namespace sim
