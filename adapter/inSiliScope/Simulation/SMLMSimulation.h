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
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "GpuSimD3D11.h"
#include "SMLMBackground.h"
#include "SMLMNoise.h"
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
   double photonsPerBlink = 2000.0;     // photons emitted per full ON frame
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
// and off at tEnd (the cell field's blink schedule, CellFieldSource).
struct BlinkEvent
{
   double xUm = 0.0;
   double yUm = 0.0;
   double zNm = 0.0; // the dye's height relative to the focus reference
   double tStart = 0.0;
   double tEnd = 0.0;
   // Per-blink photon-rate factor (log-normal, mean 1, CV FluoParam_PhotonCV).
   // Multiplies photonsPerBlink at render time.
   double brightness = 1.0;
   // The cell field's structure (ISC_STRUCT_*), state (ISC_STATE_*: a blink,
   // or a continuous window) and aux draw (issue 16, CellFieldSource).
   int structure = 0;
   int state = 0;
   double aux = 0.0;
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
   // Structured background map (photons/pixel/frame),
   // width*height, replacing the flat backgroundPhotons when non-empty.
   const std::vector<float>* backgroundMap = nullptr;
   // Background fade factor for this frame (BackgroundFadeScale).
   double backgroundScale = 1.0;
   // Render on all cores (bands of rows; the same pixels as serial). For a
   // single frame (live mode), not for frames already rendered in parallel.
   bool parallel = false;
   // Add the emitters to img as it is (no background, no resize): several
   // dye groups, each with its own PSF, into one frame (issue 16).
   bool accumulate = false;
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
// downsampling+splatting the cached oversampled diffraction PSF kernel
// (PsfGeneratorBridge.h) instead of the analytic Gaussian -- psfSigmaPx is
// then unused. Defaults to nullptr so every existing call site (Gaussian
// rendering) is unaffected.
//
// globalZOffsetUm: the focal plane's height (micrometers), driven by the
// ZStage device's shared position (Simulation/SharedStageState.h). Each
// emitter's kernel z-plane is its defocus zNm/1000 - globalZOffsetUm, so +Z
// moves the focal plane up through the sample like a real focus drive
// (until 2026-09-25 the offset was ADDED, i.e. +Z moved the emitters up).
// 0 offset + 0 zNm = in focus.
// The plane lookup is nearest-plane only, done PER EMITTER (not once per
// frame the way it used to be, back when every emitter shared one z) --
// deliberately not a two-plane blend: blending two planes' intensities is
// not the same operation as interpolating a PSF's width, so it would buy no
// accuracy while costing meaningfully more (the reference simulator this
// project tracks parity with implemented and then removed exactly this
// blend for that reason -- see docs/dev/vectorial-psf-plan.md).
//
// outZClampedCount/outZTotalCount (optional, default nullptr): accumulated
// (+=, not assigned) counts of diffraction-PSF emitter renders whose total z
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

// The GPU path's half of RenderPhotonImage: this frame's diffraction-PSF
// emitters as GpuSplatEmitter records (same photons -- overlap x brightness
// x illumination --, same per-emitter z plane, same SplatSetup), appended to
// out. The background is applied on the GPU itself. Not for Fft placement.
void CollectGpuEmitters(const std::vector<BlinkEvent>& events, long frameIndex, unsigned width, unsigned height,
                        double pixelSizeNm, double photonsPerBlink, double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache& cache, double globalZOffsetUm, const RenderExtras* extras,
                        std::vector<GpuSplatEmitter>& out, long* outZClampedCount = nullptr,
                        long* outZTotalCount = nullptr);

} // namespace sim
