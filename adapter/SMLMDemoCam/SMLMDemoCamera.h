///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMDemoCamera.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   A synthetic SMLM (Single-Molecule Localization Microscopy)
//                camera device adapter. Generates blinking-fluorophore movies
//                (Gaussian PSFs + realistic camera noise) that resolve into a
//                chosen pattern over many frames, either as a reproducible
//                precomputed stack or continuously in a live mode with
//                parameters adjustable while streaming.
//
//                Structurally based on Micro-Manager's built-in DemoCamera
//                adapter (CCameraBase, ImgBuffer, MMDeviceThreadBase sequence
//                thread) and on the always-running-producer-thread /
//                swap-buffer pattern used for decoupling simulation from
//                MM's Snap/Live pull cadence. The actual SMLM math lives in
//                Simulation/SMLMSimulation.h, which has no MMDevice
//                dependency.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "DeviceBase.h"
#include "DeviceThreads.h"
#include "ImgBuffer.h"
#include "Simulation/SMLMSimulation.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern const char* g_SMLMCameraDeviceName;

// Property name / allowed-value string constants, shared between
// SMLMDemoCamera.cpp (where they're defined and used to build the property
// list) and SMLMImageGeneration.cpp (where the AfterSet handlers compare
// against them).
extern const char* g_PropAcqMode;
extern const char* g_PropPattern;
extern const char* g_PropCustomPointsFile;
extern const char* g_PropResolutionSpacingsNm;
extern const char* g_PropFovSize;
extern const char* g_PropStackLength;
extern const char* g_PropStackLoop;
extern const char* g_PropGenerateStack;
extern const char* g_PropStackStatus;
extern const char* g_PropEndOfStack;
extern const char* g_PropEmitterDensityPerSec;
extern const char* g_PropPhotonsPerSecond;
extern const char* g_PropOnLifetimeSec;
extern const char* g_PropPsfWavelengthNm;
extern const char* g_PropPsfNa;
extern const char* g_PropPixelSize;
extern const char* g_PropBackgroundPerSec;
extern const char* g_PropQuantumEfficiency;
extern const char* g_PropDarkCurrentPerSec;
extern const char* g_PropGain;
extern const char* g_PropOffset;
extern const char* g_PropOffsetStd;
extern const char* g_PropReadNoise;
extern const char* g_PropPixelGainStdPct;
extern const char* g_PropPixelReadNoiseStdPct;
extern const char* g_PropDriftNmPerSec;
extern const char* g_PropRandomSeed;
extern const char* g_PropActualFrameIntervalMs;
extern const char* g_PropPsfModel;
extern const char* g_PropPsfImmersionIndex;
extern const char* g_PropPsfOversampling;
extern const char* g_PropPsfKernelHalfWidthPx;
extern const char* g_PropPsfGeneratorJavaHome;
extern const char* g_PropPsfZRangeUm;
extern const char* g_PropPsfZStepUm;
extern const char* g_PropPsfSampleIndex;
extern const char* g_PropPsfWorkingDistanceUm;
extern const char* g_PropPsfSampleDepthNm;

extern const char* g_PsfModelGaussian;
extern const char* g_PsfModelRichardsWolf;
extern const char* g_PsfModelGibsonLanni;

extern const char* g_AcqModePrecomputed;
extern const char* g_AcqModeLive;

extern const char* g_PatternCircle;
extern const char* g_PatternLines;
extern const char* g_PatternGrid;
extern const char* g_PatternRandom;
extern const char* g_PatternCustom;
extern const char* g_PatternSpiral;
extern const char* g_PatternStar;
extern const char* g_PatternHeart;
extern const char* g_PatternResolutionTarget;

extern const char* g_Fov128;
extern const char* g_Fov256;
extern const char* g_Fov512;

enum SMLMAcqMode
{
   SMLM_MODE_PRECOMPUTED = 0,
   SMLM_MODE_LIVE = 1,
};

class SMLMSequenceThread;

class CSMLMDemoCamera : public CCameraBase<CSMLMDemoCamera>
{
public:
   CSMLMDemoCamera();
   ~CSMLMDemoCamera();

   // MMDevice API
   // ------------
   int Initialize();
   int Shutdown();
   void GetName(char* name) const;
   bool Busy();

   // MMCamera API
   // ------------
   int SnapImage();
   const unsigned char* GetImageBuffer();
   unsigned GetImageWidth() const;
   unsigned GetImageHeight() const;
   unsigned GetImageBytesPerPixel() const;
   unsigned GetBitDepth() const;
   long GetImageBufferSize() const;
   double GetExposure() const;
   void SetExposure(double exp);
   int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize);
   int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize);
   int ClearROI();
   int StartSequenceAcquisition(double interval);
   int StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
   int StopSequenceAcquisition();
   int InsertImage();
   int RunSequenceOnThread();
   bool IsCapturing();
   void OnThreadExiting() throw();
   int GetBinning() const;
   int SetBinning(int bS);
   int IsExposureSequenceable(bool& isSequenceable) const { isSequenceable = false; return DEVICE_OK; }
   unsigned GetNumberOfComponents() const { return 1; }

   // action interface
   // ----------------
   int OnAcqMode(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPattern(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCustomPointsFile(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnResolutionSpacingsNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnFovSize(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackLength(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackLoop(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnGenerateStack(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackStatus(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEndOfStackReached(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEmitterDensityPerSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPhotonsPerSecond(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnOnLifetimeSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfWavelengthNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfNa(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPixelSizeNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBackgroundPerSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnQuantumEfficiency(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnDarkCurrentPerSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCameraGain(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCameraOffset(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnOffsetStd(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnReadNoise(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPixelGainStdPct(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPixelReadNoiseStdPct(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnDriftNmPerSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnRandomSeed(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnActualFrameIntervalMs(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfModel(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfImmersionIndex(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfOversampling(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfKernelHalfWidthPx(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfGeneratorJavaHome(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfZRangeUm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfZStepUm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfSampleIndex(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfWorkingDistanceUm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfSampleDepthNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   // Standard MM Exposure property -- this device deliberately does not add
   // any separate exposure-like property; EmitterDensityPerSec/OnLifetimeSec/
   // PhotonsPerSecond/BackgroundPerSec are all expressed as rates and scaled
   // by *this* property's current value (see SnapshotParams() in
   // SMLMImageGeneration.cpp). Changing it invalidates the precomputed stack
   // so it regenerates against the new exposure.
   int OnExposureProperty(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Called by SMLMImageGeneration.cpp / SMLMSequenceThread
   friend class SMLMSequenceThread;

private:
   // ---- sizing / ROI / binning -------------------------------------------
   unsigned FullWidth() const { return static_cast<unsigned>(cameraCCDXSize_ / binSize_); }
   unsigned FullHeight() const { return static_cast<unsigned>(cameraCCDYSize_ / binSize_); }
   sim::SimulationParams SnapshotParams() const;
   // Diffraction-limited Gaussian-PSF sigma (pixels) from emission
   // wavelength + NA + current pixel size (see SMLMImageGeneration.cpp).
   double ComputePsfSigmaPx() const;
   sim::SMLMPatternType CurrentPatternType() const { return static_cast<sim::SMLMPatternType>(patternType_); }
   sim::PsfModelKind CurrentPsfModel() const { return static_cast<sim::PsfModelKind>(psfModel_); }
   // Snapshot of everything ComputePsfKernelCache() needs to (re)compute the
   // oversampled vectorial PSF kernel via the PSFGenerator bridge -- see
   // Simulation/PsfGeneratorBridge.h. Separate from SnapshotParams()/
   // SimulationParams, which covers the blink/photon/noise model only.
   sim::PsfGeneratorRequest BuildPsfGeneratorRequest() const;
   // Called by every property handler whose value affects simulated frame
   // content (density/lifetime/photon/background rates, PSF, noise, gain,
   // offset, pattern, pixel size, binning, FOV size, exposure, seed): marks
   // the precomputed stack stale (regenerated on next use) AND bumps
   // liveConfigVersion_, which LiveProducerLoop polls every tick to know
   // when to rebuild its cached offset map / emitter pattern -- one signal
   // covering every parameter uniformly, rather than special-casing each.
   void InvalidateStack();
   // Shared by OnBinning/OnFovSize: resets ROI to the new full frame and
   // resizes img_ accordingly, then calls InvalidateStack().
   void ApplyFrameSizeChange();

   // ---- precomputed-stack mode --------------------------------------------
   void StartStackGeneration();
   void StackGenerationWorker(long stackLength, unsigned fullW, unsigned fullH,
                               sim::SimulationParams params, sim::SMLMPatternType patternType,
                               std::string customPointsFile, std::vector<double> spacingsNm, long seed,
                               sim::PsfGeneratorRequest psfRequest);
   void CropFullFrameIntoImg(const std::vector<uint16_t>& fullFrame, unsigned fullW, unsigned fullH);

   // ---- live mode -----------------------------------------------------------
   void StartLiveProducer();
   void StopLiveProducer();
   void LiveProducerLoop();

   // ---- generic frame delivery (defined in SMLMImageGeneration.cpp) --------
   // Used by SnapImage/RunSequenceOnThread. In Precomputed mode, if the stack
   // isn't ready yet, this blocks until it is (auto-triggering generation).
   // When interruptible is true (called from the sequence-acquisition
   // thread), that wait is aborted early if thd_->IsStopped() becomes true,
   // so StopSequenceAcquisition() isn't blocked for the full duration of a
   // (possibly multi-second) stack generation; returns false in that case
   // (no frame was produced -- caller should skip InsertImage this cycle).
   bool GenerateNextFrameIntoImg(bool interruptible);

   // Device state
   bool initialized_ = false;
   ImgBuffer img_;
   unsigned roiX_ = 0, roiY_ = 0, roiXSize_ = 0, roiYSize_ = 0;
   long binSize_ = 1;
   long cameraCCDXSize_ = 256;
   long cameraCCDYSize_ = 256;
   MMThreadLock imgPixelsLock_;

   int acqMode_ = SMLM_MODE_LIVE;
   int patternType_ = sim::PATTERN_CIRCLE;
   std::string customPointsFile_;
   // Ring/scale-step/spiral-arc gap (Circle/Spiral/Star/Heart) and line
   // spacing (ResolutionTarget) progression, in nanometers, easiest to
   // hardest -- see ResolutionSpacingsNm property / OnResolutionSpacingsNm.
   std::vector<double> resolutionSpacingsNm_ = sim::DefaultResolutionSpacingsNm();

   // Precomputed-stack storage
   std::vector<std::vector<uint16_t>> stack_;
   unsigned stackFrameW_ = 0, stackFrameH_ = 0;
   long stackLength_ = 1000;
   std::atomic<long> stackFramesGenerated_{0};
   std::atomic<bool> stackGenerating_{false};
   std::atomic<bool> stackReady_{false};
   long playbackIndex_ = 0;
   bool stackLoop_ = true;
   bool endOfStackReached_ = false;
   std::thread stackGenThread_;

   // Live-mode: always-running background producer thread + swap buffer,
   // decoupled from MM's Snap/Live/sequence pull cadence.
   std::thread liveProducerThread_;
   std::atomic<bool> liveProducerRun_{false};
   std::vector<uint16_t> frontFrame_, backFrame_;
   unsigned liveFrameW_ = 0, liveFrameH_ = 0;
   MMThreadLock frontFrameLock_;
   // Bumped by LiveProducerLoop every time it swaps a newly rendered frame
   // into frontFrame_. GenerateNextFrameIntoImg() (SMLM_MODE_LIVE branch)
   // compares this against lastConsumedLiveFrameSeq_ and waits for it to
   // advance instead of re-copying a frontFrame_ the producer hasn't
   // refreshed yet -- without this, a consumer pulling faster than the
   // producer's exposure-paced tick would silently deliver the same frame
   // twice (a duplicate frame) instead of waiting (a timing stutter).
   std::atomic<long> liveFrameSeq_{0};
   long lastConsumedLiveFrameSeq_ = -1;
   // Rolling average (last kFrameIntervalWindowSize publishes) of the
   // wall-clock time between successive LiveProducerLoop frame publishes --
   // i.e. what the camera is *actually* achieving, as opposed to the
   // requested Exposure. Exposed read-only via ActualFrameIntervalMs so a
   // loop that's running behind (simulation + sleep exceeding Exposure,
   // the condition that used to manifest as duplicate frames) is visible.
   static constexpr int kFrameIntervalWindowSize = 10;
   double frameIntervalHistoryMs_[kFrameIntervalWindowSize] = {};
   int frameIntervalHistoryCount_ = 0;
   int frameIntervalHistoryPos_ = 0;
   MM::MMTime lastFramePublishTime_;
   std::atomic<double> actualFrameIntervalMs_{0.0};
   sim::EmitterModel liveEmitterModel_;
   std::mt19937_64 liveRng_;
   // Atomic because StartSequenceAcquisition() (main/MMCore thread) reads it
   // to compute liveDriftOriginFrame_ while LiveProducerLoop (producer
   // thread) increments it every tick.
   std::atomic<long> liveFrameCounter_{0};
   // Value of liveFrameCounter_ at the start of the current drift ramp:
   // LiveProducerLoop computes elapsed drift time as
   // (liveFrameCounter_ - liveDriftOriginFrame_) * exposure, so setting this
   // to the current liveFrameCounter_ resets drift to zero without
   // disturbing the (unrelated) blinking-process frame clock. Reset in
   // StartLiveProducer() and at the start of every Live/MDA sequence
   // acquisition (see StartSequenceAcquisition()).
   std::atomic<long> liveDriftOriginFrame_{0};
   // Bumped by InvalidateStack() whenever any simulation-affecting property
   // changes; LiveProducerLoop compares against its own last-applied value
   // each tick to know when to rebuild its cached offset map / pattern.
   std::atomic<long> liveConfigVersion_{0};

   SMLMSequenceThread* thd_ = nullptr;
   MM::MMTime sequenceStartTime_;
   long imageCounter_ = 0;

   // Simulation parameters. Individual atomics so the live producer thread
   // and any property Set call never contend on a single lock, and every
   // parameter is genuinely adjustable while streaming.
   //
   // EmitterDensityPerSec/PhotonsPerSecond/OnLifetimeSec/BackgroundPerSec are
   // rates (per second), not per-frame quantities -- SnapshotParams() (in
   // SMLMImageGeneration.cpp) converts them to the frame-equivalent values
   // the simulation engine expects using the camera's *current* MM Exposure,
   // so they automatically scale correctly with whatever Exposure is set to.
   std::atomic<double> emitterDensityPerSec_{0.5};     // emitters / um^2 / s
   std::atomic<double> photonsPerSecond_{7500.0};      // photons / s while ON
   std::atomic<double> onLifetimeSec_{0.05};           // mean ON duration, s
   std::atomic<double> psfWavelengthNm_{660.0};        // emission wavelength, nm
   std::atomic<double> psfNa_{1.4};                    // objective numerical aperture
   std::atomic<double> pixelSizeNm_{100.0};
   std::atomic<double> backgroundPhotonsPerSec_{0.0};  // photons / pixel / s
   // Camera noise-chain defaults below (QuantumEfficiency, DarkCurrent,
   // Gain, ReadNoise) are the Photometrics Kinetix22 sCMOS, Sensitivity
   // (CMS) mode datasheet values -- the mode typically used for
   // photon-starved SMLM imaging -- per docs/vectorial-psf-plan.md's "Noise
   // model follow-ups" section: QE ~85% at this device's default 660 nm
   // emission wavelength (read off the published QE curve, not a table
   // value), 1.03 e-/pixel/sec dark current, 0.25 e-/count conversion gain,
   // 1.2 e- read noise.
   std::atomic<double> quantumEfficiency_{0.85};       // incident photons -> detected electrons
   std::atomic<double> darkCurrentPerSec_{1.03};       // e- / pixel / s, thermal
   std::atomic<double> gainPhotonsPerAdu_{0.25};
   std::atomic<double> offsetAdu_{100.0};
   // Kept below ReadNoiseElectrons (in ADU, given the default gain) so the
   // true frame-to-frame read noise -- not the static per-pixel offset
   // pattern -- dominates what a single frame visually looks like; a static
   // fixed-pattern component with std >= the temporal read noise makes
   // successive frames look like they aren't changing at all.
   std::atomic<double> offsetStdAdu_{0.5};
   std::atomic<double> readNoiseElectrons_{1.2};
   // Pixel-to-pixel relative spread of gain/read noise (sCMOS-style
   // per-pixel maps), as a percent of the nominal Gain/ReadNoise above; 0 =
   // disabled (every pixel identical), matching the driftNmPerSecX = 0
   // disabled-by-default convention used elsewhere. Photometrics doesn't
   // publish actual per-pixel variance for the Kinetix, so these two
   // defaults are estimates, not datasheet values -- see the plan doc.
   std::atomic<double> pixelGainStdPct_{5.0};
   std::atomic<double> pixelReadNoiseStdPct_{20.0};
   // Drift rate along X, nm/sec (Y drifts at half this rate -- see
   // sim::ComputeDriftOffsetPx). Applies in both acquisition modes.
   std::atomic<double> driftNmPerSecX_{0.0};

   // Vectorial PSF (embedded PSFGenerator JVM bridge, Simulation/
   // PsfGeneratorBridge.h) parameters. PsfModel gates which renderer is
   // used (Gaussian keeps the original analytic path); everything else
   // here feeds BuildPsfGeneratorRequest(). The model selector follows the
   // same plain-member convention as patternType_/customPointsFile_ (read
   // by the live producer thread without extra synchronization, same as
   // those); ImmersionIndex is atomic like the other photometric PSF
   // params (WavelengthNm/Na) it sits alongside in BuildPsfGeneratorRequest().
   int psfModel_ = static_cast<int>(sim::PsfModelKind::GibsonLanni);
   std::atomic<double> psfImmersionIndex_{1.518};
   int psfOversampling_ = 12;
   int psfKernelHalfWidthPx_ = 32;
   // JRE/JDK install root override for locating jvm.dll (empty =
   // auto-detect; see sim::FindJavaHome in PsfGeneratorBridge.cpp).
   // PSFGenerator itself and this project's bridge class are embedded in
   // this DLL -- no jar paths to configure.
   std::string psfGeneratorJavaHome_;

   // Z-stack range/step (vectorial PSF plan step 2), feeding req.nz/
   // req.zStepNm in BuildPsfGeneratorRequest(). Only matters with a
   // vectorial PsfModel. Random per-emitter Z spread (step 2) was reverted
   // in step 3 in favor of a real Z-stage device (SMLMDemoZStage.h/.cpp) --
   // see Simulation/SharedStageState.h and RenderPhotonImage's
   // globalZOffsetUm parameter, read fresh each frame in
   // StackGenerationWorker/LiveProducerLoop rather than cached here.
   std::atomic<double> psfZRangeUm_{7.0};      // total z-stack span, um
   std::atomic<double> psfZStepUm_{0.1};       // z-plane spacing, um

   // GibsonLanni-only parameters (ignored by RichardsWolf -- see PsfBridge.
   // java), previously hardcoded in PsfBridge.java rather than user-facing.
   // Defaults match what was previously hardcoded (see PsfGeneratorRequest's
   // own defaults / comment in PsfGeneratorBridge.h): SampleIndex defaults
   // to matching PsfImmersionIndex's default (1.518, not PSFGenerator's own
   // stock 1.33) and SampleDepthNm defaults to 0 (not PSFGenerator's own
   // stock 2000) -- both reproducing the no-index-mismatch, particle-
   // exactly-at-focus behavior this bridge used to force unconditionally.
   // WorkingDistanceUm defaults to 150.0, PSFGenerator's own stock default
   // for that spinner (never touched by this bridge before, so this is
   // simply making the value it was already implicitly using adjustable).
   std::atomic<double> psfSampleIndex_{1.518};
   std::atomic<double> psfWorkingDistanceUm_{150.0};
   std::atomic<double> psfSampleDepthNm_{0.0};

   long randomSeed_ = 12345;
   std::mt19937_64 rng_;
};

class SMLMSequenceThread : public MMDeviceThreadBase
{
   friend class CSMLMDemoCamera;

public:
   explicit SMLMSequenceThread(CSMLMDemoCamera* pCam);
   ~SMLMSequenceThread();
   void Stop();
   void Start(long numImages, double intervalMs);
   bool IsStopped();
   double GetIntervalMs() { return intervalMs_; }
   long GetImageCounter() { return imageCounter_; }

private:
   int svc() override;
   double intervalMs_ = 100;
   long numImages_ = 1;
   long imageCounter_ = 0;
   bool stop_ = true;
   CSMLMDemoCamera* camera_ = nullptr;
   MM::MMTime startTime_;
   MMThreadLock stopLock_;
};
