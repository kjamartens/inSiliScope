///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMImageGeneration.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Property handlers plus the thin glue between the MM camera
//                device and the standalone simulation engine
//                (Simulation/SMLMSimulation.h): precomputed-stack generation
//                (background thread), the always-running live producer
//                thread, and frame delivery into ImgBuffer.
//
// LICENSE:       BSD (see license.txt)

#include "SMLMDemoCamera.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

///////////////////////////////////////////////////////////////////////////////
// Parameter snapshot / invalidation helpers
///////////////////////////////////////////////////////////////////////////////

sim::SimulationParams CSMLMDemoCamera::SnapshotParams() const
{
   // EmitterDensityPerSec/PhotonsPerSecond/OnLifetimeSec/BackgroundPhotonsPerSec
   // are all expressed as rates (per second) at the property level, so they
   // scale correctly with whatever the standard MM Exposure is currently set
   // to -- convert them here to the frame-equivalent quantities the (unit-
   // agnostic, exposure-unaware) simulation engine expects.
   double exposureMs = GetExposure();
   double expSec = exposureMs / 1000.0;
   if (expSec <= 0.0)
      expSec = 0.001;

   sim::SimulationParams p;
   p.pixelSizeNm = pixelSizeNm_.load();
   p.emitterDensity = emitterDensityPerSec_.load() * expSec;
   p.photonsPerBlink = photonsPerSecond_.load() * expSec;
   // Clamp: an extreme (very short exposure)/(very long ON lifetime)
   // combination would otherwise blow up the lead-in window and event count
   // in GenerateAllEvents() (see SMLMSimulation.cpp), making stack
   // generation pathologically slow.
   p.onLifetimeFrames = std::min(onLifetimeSec_.load() / expSec, 20000.0);
   p.backgroundPhotons = backgroundPhotonsPerSec_.load() * expSec;
   p.psfSigmaPx = ComputePsfSigmaPx();
   p.gainPhotonsPerAdu = gainPhotonsPerAdu_.load();
   p.offsetAdu = offsetAdu_.load();
   p.offsetStdAdu = offsetStdAdu_.load();
   p.readNoiseElectrons = readNoiseElectrons_.load();
   p.driftNmPerSecX = driftNmPerSecX_.load();
   p.frameDurationSec = expSec;
   return p;
}

sim::PsfGeneratorRequest CSMLMDemoCamera::BuildPsfGeneratorRequest() const
{
   sim::PsfGeneratorRequest req;
   req.model = CurrentPsfModel();
   req.wavelengthNm = psfWavelengthNm_.load();
   req.na = psfNa_.load();
   req.immersionIndex = psfImmersionIndex_.load();
   req.pixelSizeNm = pixelSizeNm_.load();
   req.oversampling = psfOversampling_;

   // PsfKernelHalfWidthPx (2-32 camera px via its property limits) is
   // treated as a user-settable MINIMUM here, auto-grown as needed so the
   // splatted kernel always comfortably covers the first-order Airy ring
   // regardless of NA/wavelength/pixel size. Without this, a low-NA/
   // long-wavelength combination -- both within the allowed property
   // ranges -- can put the first ring outside a small fixed window,
   // silently truncating it (the rendered spot then just looks like a
   // soft square blob with no visible ring, since SplatPsfKernel simply
   // never sees data beyond the window it's given). The margin (3x the
   // classic Rayleigh first-minimum radius, 0.61*lambda/NA) comfortably
   // clears the first bright secondary maximum, mirroring the
   // physics-derived approach ComputePsfSigmaPx() already uses for the
   // Gaussian renderer. Capped at 48 px regardless of physics to keep the
   // oversampled grid PSFGenerator computes from growing unboundedly.
   double na = req.na > 0.0 ? req.na : 0.01;
   double rayleighRadiusNm = 0.61 * req.wavelengthNm / na;
   int minHalfWidthPx = static_cast<int>(std::ceil(3.0 * rayleighRadiusNm / req.pixelSizeNm));
   minHalfWidthPx = std::min(std::max(minHalfWidthPx, 2), 48);
   req.kernelHalfWidthPx = std::max(psfKernelHalfWidthPx_, minHalfWidthPx);

   // Real Z-stack (step 2): nz/zStepNm are derived from the user-facing
   // PsfZRangeUm/PsfZStepUm properties rather than hardcoded. The global
   // focus offset selecting a plane from this stack each frame comes from
   // the SMLMDemoZStage device (step 3) -- see RenderPhotonImage's
   // globalZOffsetUm parameter.
   double zRangeUm = psfZRangeUm_.load();
   double zStepUm = std::max(psfZStepUm_.load(), 0.001);
   req.nz = static_cast<int>(std::lround(zRangeUm / zStepUm)) + 1;
   req.zStepNm = zStepUm * 1000.0;

   // GibsonLanni-only (ignored by RichardsWolf); see the comments on
   // psfSampleIndex_/psfWorkingDistanceUm_/psfSampleDepthNm_ in
   // SMLMDemoCamera.h.
   req.sampleIndex = psfSampleIndex_.load();
   req.workingDistanceUm = psfWorkingDistanceUm_.load();
   req.sampleDepthNm = psfSampleDepthNm_.load();

   req.javaHome = psfGeneratorJavaHome_;
   return req;
}

double CSMLMDemoCamera::ComputePsfSigmaPx() const
{
   // Gaussian approximation of a diffraction-limited widefield PSF (Zhang et
   // al. 2007): sigma ~= 0.21 * emission_wavelength / NA.
   double wavelengthNm = psfWavelengthNm_.load();
   double na = psfNa_.load();
   if (na <= 0.0)
      na = 0.01;
   double sigmaNm = 0.21 * wavelengthNm / na;
   double sigmaPx = sigmaNm / pixelSizeNm_.load();
   // Keep it in a sane rendering range regardless of extreme wavelength/NA/
   // pixel-size combinations.
   return std::min(std::max(sigmaPx, 0.3), 20.0);
}

void CSMLMDemoCamera::InvalidateStack()
{
   stackReady_ = false;
   endOfStackReached_ = false;
   playbackIndex_ = 0;
   liveConfigVersion_.fetch_add(1, std::memory_order_relaxed);
}

void CSMLMDemoCamera::ApplyFrameSizeChange()
{
   MMThreadGuard g(imgPixelsLock_);
   roiX_ = 0;
   roiY_ = 0;
   roiXSize_ = FullWidth();
   roiYSize_ = FullHeight();
   img_.Resize(roiXSize_, roiYSize_, 2);
   InvalidateStack();
}

///////////////////////////////////////////////////////////////////////////////
// Precomputed-stack mode
///////////////////////////////////////////////////////////////////////////////

void CSMLMDemoCamera::StartStackGeneration()
{
   if (stackGenerating_.load())
      return;
   if (stackGenThread_.joinable())
      stackGenThread_.join();

   stackGenerating_ = true;
   stackFramesGenerated_ = 0;
   stackReady_ = false;
   endOfStackReached_ = false;

   unsigned fullW = FullWidth();
   unsigned fullH = FullHeight();
   sim::SimulationParams params = SnapshotParams();
   sim::SMLMPatternType patternType = CurrentPatternType();
   std::string customFile = customPointsFile_;
   std::vector<double> spacingsNm = resolutionSpacingsNm_;
   long seed = randomSeed_;
   long length = stackLength_;
   sim::PsfGeneratorRequest psfRequest = BuildPsfGeneratorRequest();

   stackGenThread_ = std::thread(&CSMLMDemoCamera::StackGenerationWorker, this, length, fullW, fullH, params,
                                  patternType, customFile, spacingsNm, seed, psfRequest);
}

void CSMLMDemoCamera::StackGenerationWorker(long stackLength, unsigned fullW, unsigned fullH,
                                             sim::SimulationParams params, sim::SMLMPatternType patternType,
                                             std::string customPointsFile, std::vector<double> spacingsNm,
                                             long seed, sim::PsfGeneratorRequest psfRequest)
{
   std::mt19937_64 localRng(static_cast<uint64_t>(seed));

   sim::EmitterModel model;
   model.SetPattern(sim::CreatePattern(patternType, customPointsFile, spacingsNm));
   model.Reseed(static_cast<uint64_t>(seed));

   double widthUm = fullW * params.pixelSizeNm / 1000.0;
   double heightUm = fullH * params.pixelSizeNm / 1000.0;

   std::vector<sim::BlinkEvent> events =
      model.GenerateAllEvents(stackLength, widthUm, heightUm, params, localRng);

   sim::PixelOffsetMap localOffsetMap;
   localOffsetMap.Generate(fullW, fullH, params.offsetAdu, params.offsetStdAdu, localRng);

   sim::PsfKernelCache localPsfCache;
   if (psfRequest.model != sim::PsfModelKind::Gaussian)
   {
      std::string err;
      if (!sim::ComputePsfKernelCache(psfRequest, localPsfCache, err))
      {
         LogMessage("Vectorial PSF unavailable, falling back to Gaussian: " + err, false);
         localPsfCache = sim::PsfKernelCache();
      }
   }

   std::vector<std::vector<uint16_t>> newStack(static_cast<size_t>(std::max(stackLength, 0L)));
   std::vector<float> photonImg;
   for (long f = 0; f < stackLength; ++f)
   {
      double dx = 0.0, dy = 0.0;
      sim::ComputeDriftOffsetPx(f * params.frameDurationSec, params.driftNmPerSecX, params.pixelSizeNm, dx, dy);
      // Read the SMLMDemoZStage device's current position fresh each frame,
      // same as any other live-adjustable parameter (see LiveProducerLoop).
      double zOffsetUm = sim::GetSharedStageState().zPositionUm.load();
      sim::RenderPhotonImage(photonImg, fullW, fullH, events, f, params.pixelSizeNm, params.psfSigmaPx,
                              params.photonsPerBlink, params.backgroundPhotons, dx, dy,
                              localPsfCache.valid ? &localPsfCache : nullptr, zOffsetUm);
      sim::ApplyNoiseChain(photonImg, newStack[static_cast<size_t>(f)], fullW, fullH,
                            params.gainPhotonsPerAdu, params.readNoiseElectrons, localOffsetMap, localRng);
      stackFramesGenerated_ = f + 1;
   }

   {
      MMThreadGuard g(imgPixelsLock_);
      stack_.swap(newStack);
      stackFrameW_ = fullW;
      stackFrameH_ = fullH;
      playbackIndex_ = 0;
      endOfStackReached_ = false;
   }

   stackReady_ = true;
   stackGenerating_ = false;
}

///////////////////////////////////////////////////////////////////////////////
// Live mode
///////////////////////////////////////////////////////////////////////////////

void CSMLMDemoCamera::StartLiveProducer()
{
   if (liveProducerRun_.load())
      return;
   if (liveProducerThread_.joinable())
      liveProducerThread_.join();

   liveFrameCounter_ = 0;
   liveDriftOriginFrame_ = 0;
   uint64_t liveSeed = static_cast<uint64_t>(randomSeed_) ^ 0xABCDEF1234567890ULL;
   liveEmitterModel_.SetPattern(sim::CreatePattern(CurrentPatternType(), customPointsFile_, resolutionSpacingsNm_));
   liveEmitterModel_.Reseed(liveSeed);
   liveRng_.seed(liveSeed);

   double pxSizeNm = pixelSizeNm_.load();
   liveEmitterModel_.ResetLive(FullWidth() * pxSizeNm / 1000.0, FullHeight() * pxSizeNm / 1000.0);

   {
      MMThreadGuard g(frontFrameLock_);
      liveFrameW_ = FullWidth();
      liveFrameH_ = FullHeight();
      frontFrame_.assign(static_cast<size_t>(liveFrameW_) * liveFrameH_, 0);
      backFrame_.assign(static_cast<size_t>(liveFrameW_) * liveFrameH_, 0);
   }
   liveFrameSeq_ = 0;
   lastConsumedLiveFrameSeq_ = -1;
   frameIntervalHistoryCount_ = 0;
   frameIntervalHistoryPos_ = 0;
   actualFrameIntervalMs_ = 0.0;

   liveProducerRun_ = true;
   liveProducerThread_ = std::thread(&CSMLMDemoCamera::LiveProducerLoop, this);
}

void CSMLMDemoCamera::StopLiveProducer()
{
   liveProducerRun_ = false;
   if (liveProducerThread_.joinable())
      liveProducerThread_.join();
}

void CSMLMDemoCamera::LiveProducerLoop()
{
   std::vector<float> photonImg;
   sim::PixelOffsetMap offsetMap;
   // Oversampled vectorial PSF kernel cache -- confined to this thread, same
   // as offsetMap, so no locking is needed. Rebuilt whenever
   // liveConfigVersion_ changes, same trigger as offsetMap/pattern below.
   sim::PsfKernelCache psfCache;
   // Sentinel: guarantees the very first tick below rebuilds both the offset
   // map and the emitter pattern/site cache, even though StartLiveProducer()
   // already primed them moments earlier (harmless redundancy, and it means
   // this loop doesn't depend on that priming being correct).
   long appliedConfigVersion = -1;

   while (liveProducerRun_.load())
   {
      MM::MMTime tickStart = GetCurrentMMTime();
      sim::SimulationParams params = SnapshotParams();
      unsigned w = FullWidth();
      unsigned h = FullHeight();
      double widthUm = w * params.pixelSizeNm / 1000.0;
      double heightUm = h * params.pixelSizeNm / 1000.0;

      // InvalidateStack() bumps liveConfigVersion_ from *every* property
      // handler that changes something affecting simulated frame content --
      // density/lifetime/photon/background rates, PSF wavelength/NA, pixel
      // size, gain, offset, offset-std, read noise, drift, pattern,
      // CustomPointsFile, binning, FOV size, exposure, random seed. A single
      // version bump (of any of them) here triggers a refresh of both
      // pieces of state Live mode caches across ticks: the static
      // fixed-pattern offset map (must match the *current* offset/offset-
      // std or frame size) and the emitter model's pattern/candidate-site
      // cache (must match the *current* pattern/custom-points-file/pixel-
      // size/FOV-size). Using one counter for all of this -- rather than
      // comparing each dependent field individually -- means a newly added
      // property only needs to call InvalidateStack() to be correctly
      // picked up live; no per-property plumbing here.
      long currentConfigVersion = liveConfigVersion_.load(std::memory_order_relaxed);
      if (currentConfigVersion != appliedConfigVersion || offsetMap.width != w || offsetMap.height != h)
      {
         offsetMap.Generate(w, h, params.offsetAdu, params.offsetStdAdu, liveRng_);
         liveEmitterModel_.SetPattern(sim::CreatePattern(CurrentPatternType(), customPointsFile_, resolutionSpacingsNm_));

         sim::PsfGeneratorRequest psfRequest = BuildPsfGeneratorRequest();
         if (psfRequest.model != sim::PsfModelKind::Gaussian)
         {
            std::string err;
            if (!sim::ComputePsfKernelCache(psfRequest, psfCache, err))
            {
               LogMessage("Vectorial PSF unavailable, falling back to Gaussian: " + err, false);
               psfCache = sim::PsfKernelCache();
            }
         }
         else
         {
            psfCache = sim::PsfKernelCache();
         }

         appliedConfigVersion = currentConfigVersion;
      }

      std::vector<sim::BlinkEvent> events =
         liveEmitterModel_.AdvanceOneFrame(liveFrameCounter_, widthUm, heightUm, params, liveRng_);

      // Drift ramps up from zero at liveDriftOriginFrame_ (reset at
      // StartLiveProducer() and at the start of every Live/MDA sequence
      // acquisition) rather than from an absolute movie length, since a
      // live stream has no fixed end to normalize against.
      long framesSinceDriftOrigin = std::max(0L, liveFrameCounter_.load(std::memory_order_relaxed) -
                                                      liveDriftOriginFrame_.load(std::memory_order_relaxed));
      double dx = 0.0, dy = 0.0;
      sim::ComputeDriftOffsetPx(framesSinceDriftOrigin * params.frameDurationSec, params.driftNmPerSecX,
                                 params.pixelSizeNm, dx, dy);
      // SMLMDemoZStage's current position, read fresh every tick so moving
      // it live in Micro-Manager sharpens/blurs the rendered PSFs in
      // real time.
      double zOffsetUm = sim::GetSharedStageState().zPositionUm.load();
      sim::RenderPhotonImage(photonImg, w, h, events, liveFrameCounter_, params.pixelSizeNm,
                              params.psfSigmaPx, params.photonsPerBlink, params.backgroundPhotons, dx, dy,
                              psfCache.valid ? &psfCache : nullptr, zOffsetUm);

      std::vector<uint16_t> nextFrame;
      sim::ApplyNoiseChain(photonImg, nextFrame, w, h, params.gainPhotonsPerAdu,
                            params.readNoiseElectrons, offsetMap, liveRng_);

      {
         MMThreadGuard g(frontFrameLock_);
         frontFrame_.swap(nextFrame);
         liveFrameW_ = w;
         liveFrameH_ = h;
      }
      liveFrameSeq_.fetch_add(1, std::memory_order_relaxed);

      MM::MMTime publishTime = GetCurrentMMTime();
      if (liveFrameCounter_ > 0)
      {
         double intervalMs = (publishTime - lastFramePublishTime_).getMsec();
         frameIntervalHistoryMs_[frameIntervalHistoryPos_ % kFrameIntervalWindowSize] = intervalMs;
         ++frameIntervalHistoryPos_;
         if (frameIntervalHistoryCount_ < kFrameIntervalWindowSize)
            ++frameIntervalHistoryCount_;
         double sum = 0.0;
         for (int i = 0; i < frameIntervalHistoryCount_; ++i)
            sum += frameIntervalHistoryMs_[i];
         actualFrameIntervalMs_.store(sum / frameIntervalHistoryCount_, std::memory_order_relaxed);
      }
      lastFramePublishTime_ = publishTime;

      ++liveFrameCounter_;

      double exposureMs = GetExposure();
      MM::MMTime elapsed = GetCurrentMMTime() - tickStart;
      double sleepMs = exposureMs - elapsed.getMsec();
      if (sleepMs > 0.0)
         CDeviceUtils::SleepMs(static_cast<unsigned long>(sleepMs));
   }
}

///////////////////////////////////////////////////////////////////////////////
// Frame delivery (shared by SnapImage / RunSequenceOnThread)
///////////////////////////////////////////////////////////////////////////////

void CSMLMDemoCamera::CropFullFrameIntoImg(const std::vector<uint16_t>& fullFrame, unsigned fullW, unsigned fullH)
{
   if (fullFrame.size() != static_cast<size_t>(fullW) * fullH)
      return;

   MMThreadGuard g(imgPixelsLock_);
   unsigned x = std::min(roiX_, fullW);
   unsigned y = std::min(roiY_, fullH);
   unsigned w = std::min(roiXSize_, fullW - x);
   unsigned h = std::min(roiYSize_, fullH - y);
   if (img_.Width() != w || img_.Height() != h || img_.Depth() != 2)
      img_.Resize(w, h, 2);

   unsigned char* dst = img_.GetPixelsRW();
   for (unsigned row = 0; row < h; ++row)
   {
      const uint16_t* srcRow = fullFrame.data() + static_cast<size_t>(y + row) * fullW + x;
      std::memcpy(dst + static_cast<size_t>(row) * w * 2, srcRow, static_cast<size_t>(w) * 2);
   }
}

bool CSMLMDemoCamera::GenerateNextFrameIntoImg(bool interruptible)
{
   if (acqMode_ == SMLM_MODE_LIVE)
   {
      // Wait for LiveProducerLoop to actually swap in a frame we haven't
      // consumed yet, rather than immediately copying frontFrame_: without
      // this, a consumer pulling faster than the producer's exposure-paced
      // tick (e.g. right after StartSequenceAcquisition, or a short
      // interval) would re-copy the still-current frontFrame_ and deliver
      // an exact duplicate. Waiting instead turns that into a timing
      // stutter -- the correct behavior, since the "duplicate" frame simply
      // hadn't been simulated yet.
      std::vector<uint16_t> frameCopy;
      unsigned w, h;
      long seq;
      for (;;)
      {
         {
            MMThreadGuard g(frontFrameLock_);
            seq = liveFrameSeq_.load(std::memory_order_relaxed);
            if (seq != lastConsumedLiveFrameSeq_)
            {
               frameCopy = frontFrame_;
               w = liveFrameW_;
               h = liveFrameH_;
               break;
            }
         }
         if (!liveProducerRun_.load())
            return false;
         if (interruptible && thd_ && thd_->IsStopped())
            return false;
         CDeviceUtils::SleepMs(1);
      }
      lastConsumedLiveFrameSeq_ = seq;
      CropFullFrameIntoImg(frameCopy, w, h);
      return true;
   }

   // Precomputed mode: auto-trigger generation on first use so a user who
   // never touches GenerateStack still gets working frames. This wait can
   // take seconds for a large stack; when called from the sequence thread
   // (interruptible), bail out early if a stop was requested in the
   // meantime so StopSequenceAcquisition() isn't blocked for the whole
   // duration of generation -- generation itself keeps running in the
   // background and will be picked up by the next Snap/sequence start.
   if (!stackReady_.load())
   {
      StartStackGeneration();
      while (!stackReady_.load())
      {
         if (interruptible && thd_ && thd_->IsStopped())
            return false;
         CDeviceUtils::SleepMs(5);
      }
   }

   std::vector<uint16_t> frame;
   unsigned fullW, fullH;
   {
      MMThreadGuard g(imgPixelsLock_);
      if (stack_.empty())
         return false;
      if (playbackIndex_ >= static_cast<long>(stack_.size()))
         playbackIndex_ = stackLoop_ ? 0 : static_cast<long>(stack_.size()) - 1;

      long idx = playbackIndex_;
      frame = stack_[static_cast<size_t>(idx)];
      fullW = stackFrameW_;
      fullH = stackFrameH_;
      if (idx == static_cast<long>(stack_.size()) - 1 && !stackLoop_)
         endOfStackReached_ = true;
      playbackIndex_++;
   }
   CropFullFrameIntoImg(frame, fullW, fullH);
   return true;
}

///////////////////////////////////////////////////////////////////////////////
// Property handlers
///////////////////////////////////////////////////////////////////////////////

int CSMLMDemoCamera::OnAcqMode(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(acqMode_ == SMLM_MODE_LIVE ? g_AcqModeLive : g_AcqModePrecomputed);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      int newMode = (s == g_AcqModeLive) ? SMLM_MODE_LIVE : SMLM_MODE_PRECOMPUTED;
      if (newMode != acqMode_)
      {
         if (newMode == SMLM_MODE_LIVE)
         {
            acqMode_ = newMode;
            if (initialized_)
               StartLiveProducer();
         }
         else
         {
            StopLiveProducer();
            acqMode_ = newMode;
         }
      }
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPattern(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* names[] = {g_PatternCircle, g_PatternLines,  g_PatternGrid,           g_PatternRandom,
                              g_PatternCustom, g_PatternSpiral, g_PatternStar,           g_PatternHeart,
                              g_PatternResolutionTarget};
      pProp->Set(names[patternType_]);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == g_PatternCircle) patternType_ = sim::PATTERN_CIRCLE;
      else if (s == g_PatternLines) patternType_ = sim::PATTERN_LINES;
      else if (s == g_PatternGrid) patternType_ = sim::PATTERN_GRID;
      else if (s == g_PatternRandom) patternType_ = sim::PATTERN_RANDOM;
      else if (s == g_PatternCustom) patternType_ = sim::PATTERN_CUSTOM_POINTS;
      else if (s == g_PatternSpiral) patternType_ = sim::PATTERN_SPIRAL;
      else if (s == g_PatternStar) patternType_ = sim::PATTERN_STAR;
      else if (s == g_PatternHeart) patternType_ = sim::PATTERN_HEART;
      else if (s == g_PatternResolutionTarget) patternType_ = sim::PATTERN_RESOLUTION_TARGET;

      // InvalidateStack() bumps liveConfigVersion_, which LiveProducerLoop
      // polls every tick and rebuilds liveEmitterModel_'s pattern from
      // accordingly -- no separate live-mode push needed here.
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnCustomPointsFile(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(customPointsFile_.c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      pProp->Get(customPointsFile_);
      // See OnPattern: InvalidateStack() alone is enough to get Live mode
      // to pick this up on its next tick.
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnResolutionSpacingsNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(sim::FormatResolutionSpacingsNm(resolutionSpacingsNm_).c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      std::vector<double> parsed = sim::ParseResolutionSpacingsNm(s);
      if (!parsed.empty())
      {
         resolutionSpacingsNm_ = parsed;
         // See OnPattern: InvalidateStack() alone is enough to get Live
         // mode to pick this up on its next tick.
         InvalidateStack();
      }
      else
      {
         // Nothing valid parsed (empty string, garbage) -- leave the
         // existing list in place and reflect that back to the caller.
         pProp->Set(sim::FormatResolutionSpacingsNm(resolutionSpacingsNm_).c_str());
      }
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnFovSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* s = (cameraCCDXSize_ == 128) ? g_Fov128 : (cameraCCDXSize_ == 256) ? g_Fov256 : g_Fov512;
      pProp->Set(s);
   }
   else if (eAct == MM::AfterSet)
   {
      if (IsCapturing())
         return DEVICE_CAMERA_BUSY_ACQUIRING;

      std::string s;
      pProp->Get(s);
      if (s == g_Fov128) { cameraCCDXSize_ = 128; cameraCCDYSize_ = 128; }
      else if (s == g_Fov256) { cameraCCDXSize_ = 256; cameraCCDYSize_ = 256; }
      else { cameraCCDXSize_ = 512; cameraCCDYSize_ = 512; }
      // Live mode's producer thread naturally re-sizes its own buffers each
      // tick (ApplyNoiseChain resizes its output to the current w*h); no
      // buffer touch needed here beyond ROI/img_ + the config-version bump.
      ApplyFrameSizeChange();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(binSize_);
   }
   else if (eAct == MM::AfterSet)
   {
      if (IsCapturing())
         return DEVICE_CAMERA_BUSY_ACQUIRING;

      long b;
      pProp->Get(b);
      if (b <= 0)
         return DEVICE_ERR;
      binSize_ = b;
      ApplyFrameSizeChange();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnStackLength(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(stackLength_);
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      stackLength_ = v;
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnStackLoop(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(stackLoop_ ? "On" : "Off");
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      stackLoop_ = (s == "On");
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnGenerateStack(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(0L);
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      if (v != 0)
      {
         StartStackGeneration();
         pProp->Set(0L);
      }
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnStackStatus(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      if (stackGenerating_.load())
      {
         std::ostringstream os;
         os << "Generating " << stackFramesGenerated_.load() << "/" << stackLength_;
         pProp->Set(os.str().c_str());
      }
      else if (stackReady_.load())
      {
         std::ostringstream os;
         os << "Ready (" << stack_.size() << " frames)";
         pProp->Set(os.str().c_str());
      }
      else
      {
         pProp->Set("Idle");
      }
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnEndOfStackReached(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(endOfStackReached_ ? "Yes" : "No");
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnEmitterDensityPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(emitterDensityPerSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); emitterDensityPerSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPhotonsPerSecond(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(photonsPerSecond_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); photonsPerSecond_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnOnLifetimeSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(onLifetimeSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); onLifetimeSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfWavelengthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfWavelengthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfWavelengthNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfNa(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfNa_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfNa_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPixelSizeNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(pixelSizeNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); pixelSizeNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnBackgroundPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(backgroundPhotonsPerSec_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); backgroundPhotonsPerSec_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnCameraGain(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(gainPhotonsPerAdu_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); gainPhotonsPerAdu_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnCameraOffset(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(offsetAdu_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); offsetAdu_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnOffsetStd(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(offsetStdAdu_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); offsetStdAdu_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnReadNoise(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(readNoiseElectrons_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); readNoiseElectrons_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnDriftNmPerSec(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(driftNmPerSecX_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); driftNmPerSecX_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnRandomSeed(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(randomSeed_);
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      randomSeed_ = v;
      rng_.seed(static_cast<uint64_t>(randomSeed_));
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnActualFrameIntervalMs(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(actualFrameIntervalMs_.load(std::memory_order_relaxed));
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfModel(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      const char* names[] = {g_PsfModelGaussian, g_PsfModelRichardsWolf, g_PsfModelGibsonLanni};
      pProp->Set(names[psfModel_]);
   }
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == g_PsfModelGaussian) psfModel_ = static_cast<int>(sim::PsfModelKind::Gaussian);
      else if (s == g_PsfModelRichardsWolf) psfModel_ = static_cast<int>(sim::PsfModelKind::RichardsWolf);
      else if (s == g_PsfModelGibsonLanni) psfModel_ = static_cast<int>(sim::PsfModelKind::GibsonLanni);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfImmersionIndex(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfImmersionIndex_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfImmersionIndex_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfOversampling(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(static_cast<long>(psfOversampling_));
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      if (v < 1)
         v = 1;
      psfOversampling_ = static_cast<int>(v);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfKernelHalfWidthPx(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(static_cast<long>(psfKernelHalfWidthPx_));
   }
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      if (v < 1)
         v = 1;
      psfKernelHalfWidthPx_ = static_cast<int>(v);
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfGeneratorJavaHome(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(psfGeneratorJavaHome_.c_str());
   }
   else if (eAct == MM::AfterSet)
   {
      pProp->Get(psfGeneratorJavaHome_);
      // Only takes effect before the first vectorial-PSF computation: the
      // embedded JVM is created once per process (JNI only supports one
      // JVM per process, see sim::EnsureJvmCreated) and reused for the
      // rest of this device adapter's lifetime, so changing this after
      // that first use has no effect until Micro-Manager/the process is
      // restarted. InvalidateStack() is still called for consistency with
      // every other PSF-affecting property, even though it's a no-op here
      // post-JVM-creation.
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfZRangeUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfZRangeUm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfZRangeUm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfZStepUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfZStepUm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfZStepUm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfSampleIndex(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfSampleIndex_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfSampleIndex_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfWorkingDistanceUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfWorkingDistanceUm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfWorkingDistanceUm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnPsfSampleDepthNm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet) pProp->Set(psfSampleDepthNm_.load());
   else if (eAct == MM::AfterSet) { double v; pProp->Get(v); psfSampleDepthNm_ = v; InvalidateStack(); }
   return DEVICE_OK;
}

int CSMLMDemoCamera::OnExposureProperty(MM::PropertyBase* /*pProp*/, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
      // EmitterDensityPerSec/OnLifetimeSec/PhotonsPerSecond/
      // BackgroundPhotonsPerSec are all rates converted to frame-equivalent
      // values using this property's current value (SnapshotParams()) -- a
      // changed Exposure means the precomputed stack no longer reflects the
      // current settings and must regenerate.
      InvalidateStack();
   }
   return DEVICE_OK;
}
