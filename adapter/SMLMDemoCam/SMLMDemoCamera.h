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
#include "Simulation/SMLMStructures.h"
#include "Simulation/SMLMZernike.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
extern const char* g_PropPsfKernelHalfWidthNm;
extern const char* g_PropPsfGeneratorJavaHome;
extern const char* g_PropPsfZRangeUm;
extern const char* g_PropPsfZStepUm;
extern const char* g_PropPsfSampleIndex;
extern const char* g_PropPsfWorkingDistanceUm;
extern const char* g_PropPsfSampleDepthNm;
extern const char* g_PropPsfZernikeCoefficients;
extern const char* g_PropPsfZernikePreset;

// 3D structures / labeling efficiency -- see Simulation/SMLMStructures.h.
extern const char* g_PropLabelingEfficiencyPct;
extern const char* g_PropStructureZRangeNm;
extern const char* g_PropStructureSizeNm;
extern const char* g_PropNupRadiusNm;
extern const char* g_PropNupCornerSpreadNm;
extern const char* g_PropNupRingSeparationNm;
extern const char* g_PropNupLinkerMinNm;
extern const char* g_PropNupLinkerMaxNm;
extern const char* g_PropNupMembraneType;
extern const char* g_PropNupCount;
extern const char* g_PropNupMinSpacingNm;
extern const char* g_PropNupCurvatureNm;

extern const char* g_NupMembraneTopDown;
extern const char* g_NupMembraneSideways;

// Multi-blink photophysics, illumination, EMCCD and structured background
// (webSMLM parity round 2) -- see the members' comments below.
extern const char* g_PropBlinkBleachProb;
extern const char* g_PropOffLifetimeSec;
extern const char* g_PropPhotonCV;
extern const char* g_PropIllumFwhmPct;
extern const char* g_PropEmGain;
extern const char* g_PropCicElectrons;
extern const char* g_PropBgCellContrast;
extern const char* g_PropBgHazeWeight;
extern const char* g_PropBgHazeWidthNm;
extern const char* g_PropBgDecaySec;
extern const char* g_PropOutOfFocusRatio;
extern const char* g_PropOutOfFocusDepthNm;
extern const char* g_PropIllumProfile;
extern const char* g_IllumFlat;
extern const char* g_IllumGaussian;
extern const char* g_IllumFlatTop;
extern const char* g_PropCameraType;
extern const char* g_CameraTypeScmos;
extern const char* g_CameraTypeEmccd;
extern const char* g_PropBitDepth;

// Sub-pixel PSF placement -- see Simulation/PsfGeneratorBridge.h's
// PsfInterpMode.
extern const char* g_PropPsfInterp;
extern const char* g_PsfInterpNearest;
extern const char* g_PsfInterpLinear;
extern const char* g_PsfInterpCubic;
extern const char* g_PsfInterpFft;

// GPU (Direct3D 11) splat + noise path -- see Simulation/GpuSimD3D11.h.
extern const char* g_PropUseGpu;
extern const char* g_PropGpuStatus;
extern const char* g_UseGpuOn;
extern const char* g_UseGpuOff;

// GibsonLanniZernike-only pupil phase mask -- see Simulation/
// PsfGeneratorBridge.h's PsfMaskType.
extern const char* g_PropPsfMaskType;
extern const char* g_PropPsfMaskModes;
extern const char* g_PropPsfMaskWaist;
extern const char* g_PsfMaskNone;
extern const char* g_PsfMaskDoubleHelix;

extern const char* g_PsfModelGaussian;
extern const char* g_PsfModelRichardsWolf;
extern const char* g_PsfModelGibsonLanni;
extern const char* g_PsfModelGibsonLanniZernike;

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
extern const char* g_PatternTiltedPlane;
extern const char* g_PatternUniform3D;
extern const char* g_PatternShell;
extern const char* g_PatternNup;
extern const char* g_PatternCalibration9Spots;
extern const char* g_PatternFilamentsRing;

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
   int OnPsfKernelHalfWidthNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfGeneratorJavaHome(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfZRangeUm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfZStepUm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfSampleIndex(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfWorkingDistanceUm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfSampleDepthNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfZernikeCoefficients(MM::PropertyBase* pProp, MM::ActionType eAct);
   // Applies a named literature-inspired aberration template (see
   // Simulation/SMLMZernike.h's ZernikePresetCoefficients) to
   // PsfZernikeCoefficients -- a convenience on top of it, not a separate
   // source of truth: BeforeGet just reports the last-applied name;
   // AfterSet overwrites psfZernikeCoefficients_ and notifies the GUI via
   // OnPropertyChanged so the PsfZernikeCoefficients property reflects the
   // resolved values.
   int OnPsfZernikePreset(MM::PropertyBase* pProp, MM::ActionType eAct);
   // 3D structures / labeling efficiency -- see Simulation/SMLMStructures.h
   // and BuildStructureParams() below.
   int OnLabelingEfficiencyPct(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStructureZRangeNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStructureSizeNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupRadiusNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupCornerSpreadNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupRingSeparationNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupLinkerMinNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupLinkerMaxNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupMembraneType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupCount(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupMinSpacingNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnNupCurvatureNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfInterp(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnUseGpu(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnGpuStatus(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBlinkBleachProb(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnOffLifetimeSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPhotonCV(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnIllumFwhmPct(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEmGain(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCicElectrons(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBgCellContrast(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBgHazeWeight(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBgHazeWidthNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBgDecaySec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnOutOfFocusRatio(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnOutOfFocusDepthNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnIllumProfile(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCameraType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBitDepth(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfMaskType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfMaskModes(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfMaskWaist(MM::PropertyBase* pProp, MM::ActionType eAct);
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
   // Snapshot of everything BuildStructurePattern() needs (Simulation/
   // SMLMStructures.h) -- separate from SnapshotParams()/
   // BuildPsfGeneratorRequest() for the same reason those two are separate
   // from each other: distinct lifetimes (rebuilt only on config change,
   // via CreatePattern, never per frame) and a distinct consumer.
   sim::StructureParams BuildStructureParams() const;
   // Illumination field + structured background map for the current
   // settings (Simulation/SMLMBackground.h), each drawn from its own
   // seed-derived rng stream; empty fields when the features are off.
   sim::StackShapingFields BuildShapingFields(const sim::EmitterModel& model, unsigned w, unsigned h,
                                              const sim::SimulationParams& params, long seed) const;
   // Depth sampler for the out-of-focus population (Background_OutOfFocus-
   // DepthNm), clamped to the cache's z range; empty if the cache has no z
   // stack (then the population is skipped).
   std::function<double(std::mt19937_64&)> OutOfFocusDepthSampler(const sim::PsfKernelCache& cache) const;
   // Creates (if gpu is empty) and loads a GPU simulator with this
   // kernel/maps/background, when General_UseGpu is On and the frame can be
   // rendered on the GPU at all (vectorial kernel, not Fft placement).
   // Returns false -- the caller then renders on the CPU -- otherwise, or on
   // any D3D11 failure (logged once, and reported by General_GpuStatus).
   bool PrepareGpu(std::unique_ptr<sim::GpuSimulator>& gpu, const sim::PsfKernelCache& cache, unsigned w,
                   unsigned h, const sim::PixelOffsetMap& offsetMap, const sim::PixelGainMap& gainMap,
                   const sim::PixelReadNoiseMap& readNoiseMap, const sim::StackShapingFields& shaping,
                   const sim::SimulationParams& params);
   void SetGpuStatus(const std::string& s);
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
                               sim::PsfGeneratorRequest psfRequest, sim::StructureParams structure);
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
   // No longer user-facing MM properties (removed): a precomputed stack is
   // always this long and always loops. Kept as members rather than being
   // inlined at their use sites so the playback/generation code below reads
   // unchanged, and so re-exposing either is a one-line property add.
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
   // Out-of-focus population (Background_OutOfFocusRatio): its own model
   // (it carries in-flight events across ticks) and its own rng stream, so
   // enabling it never shifts the in-focus stream.
   sim::EmitterModel liveOutOfFocusModel_;
   std::mt19937_64 liveOutOfFocusRng_;
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
   // Drift speed, nm/sec, along a direction drawn once per RandomSeed (see
   // sim::ComputeDriftOffsetPx/DriftAngleForSeed). Applies in both
   // acquisition modes. (The member name predates the random direction,
   // when this was the X rate of a fixed X:Y = 2:1 diagonal.)
   std::atomic<double> driftNmPerSecX_{0.0};

   // ---- webSMLM parity round 2 -- every default below is "off", matching
   // webSMLM's realism=min, so a default movie is unchanged by them. ----
   // Multi-blink photophysics (sim::SimulationParams::blinkBleachProb etc.):
   // bleach probability per blink (1 = the original single-blink model),
   // mean dark time between blinks (a rate property like OnLifetimeSec,
   // converted to frames in SnapshotParams; 1 s = webSMLM's default 20
   // frames at the default 50 ms exposure), and per-blink photon-rate CV.
   std::atomic<double> blinkBleachProb_{1.0};
   std::atomic<double> offLifetimeSec_{1.0};
   std::atomic<double> photonCV_{0.0};
   // Excitation illumination profile (sim::IllumProfile, SMLMBackground.h),
   // peak-normalized; FWHM as percent of the FOV width.
   int illumProfile_ = static_cast<int>(sim::IllumProfile::Flat);
   std::atomic<double> illumFwhmPct_{60.0};
   // Sensor: sCMOS (the original chain) or EMCCD (sim::CameraNoiseParams).
   // EmGain/Cic/BitDepth only matter for EMCCD; webSMLM's defaults.
   bool cameraEmccd_ = false;
   std::atomic<double> emGain_{300.0};
   std::atomic<double> cicElectrons_{0.002};
   int bitDepth_ = 16;
   // Structured background (sim::BuildBackgroundMap): cell contrast (1 =
   // flat), out-of-focus haze weight (0 = none) and blur width, and the
   // fade-to-30%-floor time constant in seconds (0 = no fade).
   std::atomic<double> bgCellContrast_{1.0};
   std::atomic<double> bgHazeWeight_{0.0};
   std::atomic<double> bgHazeWidthNm_{800.0};
   std::atomic<double> bgDecaySec_{0.0};
   // Blinking out-of-focus emitters: a second population on the same
   // structure at OutOfFocusRatio x the in-focus density, placed 300 nm to
   // OutOfFocusDepthNm above/below focus and rendered through the real
   // defocused vectorial PSF (needs a vectorial PsfModel). 0 = none.
   std::atomic<double> outOfFocusRatio_{0.0};
   std::atomic<double> outOfFocusDepthNm_{1500.0};

   // 3D structures / labeling efficiency (Simulation/SMLMStructures.h).
   // StructureZRangeNm defaults to 500 (not 0) so 3D structures/spread are
   // visible out of the box -- see CLAUDE.md and docs/vectorial-psf-plan.md.
   // LabelingEfficiencyPct's 70 (not 100) is likewise a deliberate "look
   // like a real experiment out of the box" default, not a neutral one.
   std::atomic<double> labelingEfficiencyPct_{70.0};
   std::atomic<double> structureZRangeNm_{500.0};
   std::atomic<double> structureSizeNm_{500.0};
   std::atomic<double> nupRadiusNm_{53.5};
   std::atomic<double> nupCornerSpreadNm_{12.0};
   std::atomic<double> nupRingSeparationNm_{50.0};
   std::atomic<double> nupLinkerMinNm_{2.0};
   std::atomic<double> nupLinkerMaxNm_{5.0};
   // Plain member, same convention as patternType_/psfModel_ (read by the
   // live producer thread without extra synchronization).
   int nupMembrane_ = static_cast<int>(sim::MembraneOrientation::TopDown);
   int nupCount_ = 80;
   std::atomic<double> nupMinSpacingNm_{200.0};
   std::atomic<double> nupCurvatureNm_{150.0};

   // Sub-pixel PSF splat sampling mode -- Linear/Cubic remove the
   // 1/oversampling placement quantization; Nearest reproduces the original
   // box-average behavior exactly. See Simulation/PsfGeneratorBridge.h's
   // PsfInterpMode. Plain member, same convention as psfModel_/patternType_.
   int psfInterp_ = static_cast<int>(sim::PsfInterpMode::Cubic);
   // Render vectorial-PSF frames (splat + noise) on the GPU when one is
   // available (General_UseGpu); gpuStatus_ is what General_GpuStatus reports
   // -- the adapter in use, or why the CPU is being used.
   bool useGpu_ = true;
   std::mutex gpuStatusMutex_;
   std::string gpuStatus_ = "Not used yet";
   // GibsonLanniZernike-only pupil phase mask (Simulation/PsfGeneratorBridge.h's
   // PsfMaskType) and its Gauss-Laguerre mode count / waist (pupil radii).
   // Defaults are webSMLM's: no mask, 5 modes, waist 1.0.
   int psfMaskType_ = static_cast<int>(sim::PsfMaskType::None);
   int psfMaskModes_ = 5;
   std::atomic<double> psfMaskWaist_{1.0};

   // Vectorial PSF (embedded PSFGenerator JVM bridge, Simulation/
   // PsfGeneratorBridge.h) parameters. PsfModel gates which renderer is
   // used (Gaussian keeps the original analytic path); everything else
   // here feeds BuildPsfGeneratorRequest(). The model selector follows the
   // same plain-member convention as patternType_/customPointsFile_ (read
   // by the live producer thread without extra synchronization, same as
   // those); ImmersionIndex is atomic like the other photometric PSF
   // params (WavelengthNm/Na) it sits alongside in BuildPsfGeneratorRequest().
   int psfModel_ = static_cast<int>(sim::PsfModelKind::GibsonLanniZernike);
   std::atomic<double> psfImmersionIndex_{1.518};
   // Oversampling and the kernel half-width below together set the
   // oversampled kernel's pixel count, which is O((halfWidthPx*oversampling)^2)
   // and is by far the dominant cost of a (re)compute -- see
   // GibsonLanniZernikePSF's class Javadoc. Step 5 lowered these to 4 and
   // 32px from 12 and 32px for exactly that reason; they sit higher again
   // now (6 and 3000nm = 30px at the default pixel size) because that model
   // is evaluated by chirp-Z transform (the slower, wide-kernel-incorrect
   // Direct evaluator has been removed).
   int psfOversampling_ = 6;
   // Kernel half-width in NANOMETERS -- converted to a whole camera-pixel
   // count against the current PixelSizeNm in BuildPsfGeneratorRequest(),
   // where it remains a MINIMUM that the NA/wavelength-derived Airy margin
   // can grow further. Atomic like the other photometric PSF params it now
   // sits alongside (it used to be a plain int pixel count).
   std::atomic<double> psfKernelHalfWidthNm_{3000.0};
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

   // GibsonLanniZernike-only: PsfZernikeCoefficients (28-value comma-
   // separated positional list, OSA index 0-27 -- see Simulation/
   // SMLMZernike.h) and the last-applied PsfZernikePreset name (purely a
   // convenience label; PsfZernikeCoefficients is the actual value read by
   // BuildPsfGeneratorRequest -- see SMLMImageGeneration.cpp). Plain
   // std::string, not std::atomic<std::string> (not specializable), same
   // convention as psfGeneratorJavaHome_ above.
   // Both default to the MixedRealisticObjective preset rather than "None"/
   // all-zero, so the default GibsonLanniZernike model actually shows the
   // aberrated PSF it exists to model out of the box. The two must agree:
   // psfZernikeCoefficients_ is the value BuildPsfGeneratorRequest actually
   // reads, psfZernikePreset_ only labels where it came from.
   std::string psfZernikePreset_ = "MixedRealisticObjective";
   std::string psfZernikeCoefficients_ =
      sim::FormatZernikeCoefficients(sim::ZernikePresetCoefficients("MixedRealisticObjective"));

   long randomSeed_ = 42;
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
