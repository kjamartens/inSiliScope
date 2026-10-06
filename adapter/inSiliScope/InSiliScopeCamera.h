///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeCamera.h
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
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "DeviceBase.h"
#include "DeviceThreads.h"
#include "ImgBuffer.h"
#include "Simulation/BrightfieldRender.h"
#include "Simulation/CellFieldSource.h"
#include "Simulation/IlluminationHistory.h"
#include "Simulation/ScopeMovie.h"
#include "Simulation/SMLMSimulation.h"
#include "Simulation/SharedStageState.h"
#include "Simulation/SMLMZernike.h"
#include "Simulation/WidefieldGpuD3D11.h"
#include "Simulation/WidefieldRender.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern const char* g_CameraDeviceName;

// Property name / allowed-value string constants, shared between
// InSiliScopeCamera.cpp (where they're defined and used to build the property
// list) and SMLMImageGeneration.cpp (where the AfterSet handlers compare
// against them).
extern const char* g_PropAcqMode;
extern const char* g_PropFovSize;
extern const char* g_PropGenerateStack;
extern const char* g_PropStackStatus;
extern const char* g_PropStackLength;
extern const char* g_PropEndOfStack;
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
extern const char* g_PropDriftXyNmPerSqrtSec;
extern const char* g_PropDriftZNmPerSqrtSec;
// Directed sample drift (sim::DriftSettings). Only the two speeds are
// everyday settings; direction, wanders and their time are advanced.
enum DirectedDriftNumber
{
   DD_XY_SPEED = 0,   // SimType_DriftXySpeedNmPerSec
   DD_Z_SPEED,        // SimType_DriftZSpeedNmPerSec (signed)
   DD_XY_ANGLE,       // SimType_DriftXyAngleDeg (-1 = random per seed; advanced)
   DD_ANGLE_WANDER,   // SimType_DriftXyAngleWanderDeg (advanced)
   DD_SPEED_WANDER,   // SimType_DriftSpeedWanderPct (advanced)
   DD_WANDER_TIME,    // SimType_DriftWanderTimeSec (advanced)
   DD_COUNT
};
extern const char* g_PropDirectedDrift[DD_COUNT];
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

// Illumination profile (Optics_), EMCCD and background fade (webSMLM parity
// round 2) -- see the members' comments below.
extern const char* g_PropIllumFwhmPct;
extern const char* g_PropEmGain;
extern const char* g_PropCicElectrons;
extern const char* g_PropBgDecaySec;
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

// CellField pattern (the insiliscope world, spec/PORT.md 9): numeric
// properties share one indexed handler (OnCellFieldNumber), in this order.
enum CellFieldNumber
{
   CF_CHUNK_SIZE_UM = 0,
   CF_OCCUPANCY,
   CF_CELL_DIAM_MIN_UM,
   CF_CELL_DIAM_MAX_UM,
   CF_MT_DENSITY,
   CF_FOCUS_HEIGHT_UM,
   CF_Z_RANGE_UM,
   // Nucleus shape and microtubule start/end (2026-10-05): core parameters of the same names (CellFieldParamNames).
   CF_NUC_BASE_MIN_UM,
   CF_NUC_BASE_MAX_UM,
   CF_NUC_IRREG_MIN,
   CF_NUC_IRREG_MAX,
   CF_NUC_BEND_MIN,
   CF_NUC_BEND_MAX,
   CF_NUC_SMOOTH,
   CF_NUC_THICK_IRREG,
   CF_NUC_ASYM,
   CF_NUC_WIDEST_MIN,
   CF_NUC_WIDEST_MAX,
   CF_MT_START_DECAY_PCT,
   CF_MT_END_DECAY_PCT,
   CF_MT_DIR_KAPPA,
   CF_COUNT
};
extern const char* g_PropCellFieldNumber[CF_COUNT];
// The core parameter each CellField number sets as is (nullptr: handled by hand in BuildCellFieldSettings).
extern const char* g_CellFieldCoreParam[CF_COUNT];
extern const char* g_PropCellFieldPacking;
extern const char* g_PropDiskCache;
extern const char* g_DiskCacheOff;
extern const char* g_DiskCacheCells;
extern const char* g_DiskCacheCellsAndPsf;

// Imaging modality (General_ImagingModality): Fluorescence renders every
// structure's label in its mode through the light path (issue 16; the
// engine's FluorescenceMovie), BrightField transmitted light. The mean-field
// grid's numbers (General_WideFieldUpscaling/ZPlaneNm) share one indexed
// handler (OnWideFieldNumber), in this order.
extern const char* g_PropImagingModality;
extern const char* g_ModalityFluorescence;
enum WideFieldNumber
{
   WF_UPSCALING = 0,
   WF_Z_PLANE_NM,
   WF_COUNT
};
extern const char* g_PropWideFieldNumber[WF_COUNT];
// BrightField (transmitted light, Simulation/BrightfieldRender.h; CellField
// only): its numeric properties, one indexed handler (OnBrightFieldNumber).
// Quality 1-4 sets sources/upscaling/geometry samples/slice step unless
// those are set (> 0; slice >= 0).
extern const char* g_ModalityBrightField;
enum BrightFieldNumber
{
   BF_QUALITY = 0,
   BF_SOURCES,
   BF_UPSCALING,
   BF_GEOMETRY_SAMPLES,
   BF_SLICE_UM,
   BF_CONDENSER_NA,
   BF_WAVELENGTH_NM,
   BF_PHOTONS_PER_PX_PER_SEC,
   BF_ABERRATIONS,
   BF_INDEX_MEDIUM,
   BF_INDEX_CYTOPLASM,
   BF_INDEX_NUCLEUS,
   BF_INDEX_MICROTUBULE,
   BF_ABSORPTION_PER_UM,
   BF_COUNT
};
extern const char* g_PropBrightFieldNumber[BF_COUNT];

extern const char* g_Fov128;
extern const char* g_Fov256;
extern const char* g_Fov512;

enum SMLMAcqMode
{
   SMLM_MODE_PRECOMPUTED = 0,
   SMLM_MODE_LIVE = 1,
};

class SMLMSequenceThread;

class CInSiliScopeCamera : public CCameraBase<CInSiliScopeCamera>
{
public:
   CInSiliScopeCamera();
   ~CInSiliScopeCamera();

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
   int OnFovSize(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnGenerateStack(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackStatus(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackLength(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEndOfStackReached(MM::PropertyBase* pProp, MM::ActionType eAct);
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
   int OnDriftXyNmPerSqrtSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnDriftZNmPerSqrtSec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnDirectedDrift(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
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
   int OnPsfInterp(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnUseGpu(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnGpuStatus(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnIllumFwhmPct(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEmGain(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCicElectrons(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBgDecaySec(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnIllumProfile(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCameraType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBitDepth(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfMaskType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfMaskModes(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPsfMaskWaist(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCellFieldNumber(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
   int OnCellFieldPacking(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnDiskCache(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnImagingModality(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnWideFieldNumber(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
   int OnBrightFieldNumber(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
   // Issue 16 (ScopeProperties.cpp): a property that is one engine scope
   // option (optionProps_[index]); a dye field of the microtubules' dye or a
   // slot (dyeFieldProps_[index]); Optics_Preset; CamParam_CameraPreset; the
   // read-only label readouts (0 detected %, 1 wavelength, 2 photons/s).
   int OnScopeOption(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
   int OnDyeField(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
   int OnLightPreset(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnCameraPreset(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnLabelReadout(MM::PropertyBase* pProp, MM::ActionType eAct, long index);
   // Standard MM Exposure property -- this device deliberately does not add
   // any separate exposure-like property; the dyes' rates and
   // BackgroundPerSec are all expressed per second and scaled
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
   // ---- issue 16 (ScopeProperties.cpp): the engine's scope spec ----
   // The properties as a ScopeSpec (ScopeMovie.h) for a FOV centred on the
   // stage (x, y) with the Z stage at z, from simulated time startSec, frames
   // long: what FluorescenceMovie renders (the cli/viewer's options).
   sim::ScopeSpec BuildScopeSpec(double stageXUm, double stageYUm, double zStageUm, double startSec, long frames) const;
   void CreateScopeProperties();
   // A dye pick or label mode change: the microtubules' dye fields reload from
   // the library, the labelled sites take the mode's suggestion (on a mode
   // change) and the light path the dye mode's light preset.
   void LoadMicrotubuleDye(bool modeMayChange);
   // ---- the illumination history (Simulation/IlluminationHistory.h) ----
   // The rect a fluorescence frame lights at stage (x, y): the FOV, its
   // 2 um margin (what the movie renders) and 0.5 um of slack, world um.
   void LitRect(double stageXUm, double stageYUm, double& x0, double& y0, double& x1, double& y1) const;
   // The illumination profile at a world position as a dose weight (peak 1,
   // in 1/16 steps so the clocks of a profile form few regions; the margin
   // takes the nearest FOV pixel); empty for the flat profile.
   std::function<double(double, double)> HistoryWeight(const sim::StackShapingFields& shaping, double stageXUm,
                                                       double stageYUm) const;
   // Clears the history when the world (seed, cells, packing) changed.
   void SyncHistoryWorld();
   void ApplyLightPreset(const std::string& id);
   void ApplyCameraPreset(int index);
   // The camera preset's gain for the current imaging (sim::CameraPresetGain).
   void ApplyModeGain();
   double EmGain() const;   // the camera preset's pre-amplifier sensitivity / the gain
   void NotifyOption(const std::string& option);
   double OptionValue(const std::string& option) const;
   void SetOptionValue(const std::string& option, double v);
   sim::PsfModelKind CurrentPsfModel() const { return static_cast<sim::PsfModelKind>(psfModel_); }
   // Snapshot of everything ComputePsfKernelCache() needs to (re)compute the
   // oversampled diffraction PSF kernel via the PSFGenerator bridge -- see
   // Simulation/PsfGeneratorBridge.h. Separate from SnapshotParams()/
   // SimulationParams, which covers the blink/photon/noise model only.
   sim::PsfGeneratorRequest BuildPsfGeneratorRequest() const;
   // Illumination field for the current settings (Simulation/SMLMBackground.h);
   // empty when flat.
   sim::StackShapingFields BuildShapingFields(unsigned w, unsigned h) const;
   // CellField pattern: the world/kinetics settings, and the query for a FOV
   // of w x h pixels centred on the XY stage position (stageX, stageY) with
   // the Z stage at zStageUm, over simulated [tSec, tSec + spanSec). The
   // query rect is the FOV shifted against the drift at the start and end of
   // that span (drift px, the renderer adds it back) plus a PSF margin; dyes
   // beyond +/- SimType_CellFieldZRangeUm/2 of the focal plane are culled.
   sim::CellFieldSettings BuildCellFieldSettings() const;
   // Corelog warning when the diffraction kernel's z range cannot cover the
   // SimType_CellFieldZRangeUm slab (the renderer then clamps those dyes to
   // the kernel's end plane); empty otherwise.
   std::string CellFieldZRangeWarning() const;
   sim::CellFieldQuery CellFieldQueryFor(double stageX, double stageY, double zStageUm, unsigned w, unsigned h,
                                         const sim::SimulationParams& params, double drift0XPx, double drift0YPx,
                                         double drift1XPx, double drift1YPx, long frameIndex, double tSec,
                                         double spanSec) const;
   // BrightField: the modality property is BrightField.
   bool BrightFieldSelected() const { return modality_.load() == 1; }
   // The QE of the camera's curve at the BrightField lamp wavelength.
   double BrightFieldQe() const;
   // The BrightField spec (optics, quality, specimen indices) for the current
   // properties at the pose of q (FOV origin), w x h pixels.
   sim::BrightfieldSpec BuildBrightfieldSpec(const sim::SimulationParams& params, const sim::CellFieldQuery& q,
                                             unsigned w, unsigned h) const;
   // BrightField precomputed stack: one image per distinct focus, times the
   // lamp flux, camera noise per frame; Z is read per batch of frames.
   void RenderBrightfieldStack(std::vector<std::vector<uint16_t>>& stack, long stackLength, unsigned w, unsigned h,
                               const sim::SimulationParams& params, const sim::CellFieldSettings& cellField,
                               double stageXUm, double stageYUm, const sim::PixelOffsetMap& offsetMap,
                               const sim::PixelGainMap& gainMap, const sim::PixelReadNoiseMap& readNoiseMap,
                               uint32_t noiseSeed, const std::vector<sim::DriftNm>& drift);
   // The calling thread's WideField GPU host (created once: gpu/tried are
   // the thread's), or nullptr for the CPU; sets General_GpuStatus.
   sim::WidefieldGpuD3D11* WideFieldGpu(std::unique_ptr<sim::WidefieldGpuD3D11>& gpu, bool& tried);
   // Creates (if gpu is empty) and loads a GPU simulator with this
   // kernel/maps/background, when General_UseGpu is On and the frame can be
   // rendered on the GPU at all (diffraction kernel, not Fft placement).
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
   // Marks the precomputed stack stale WITHOUT bumping liveConfigVersion_ --
   // for properties (currently just Exposure) that the precomputed-stack
   // frame-equivalent conversion depends on but that LiveProducerLoop's
   // per-tick SnapshotParams() already picks up fresh with no rebuild
   // needed (its cached offset map / emitter pattern / PSF kernel do not
   // depend on exposure time). Using the full InvalidateStack() here would
   // force a PSF-kernel recompute (seconds, for diffraction models) on every
   // exposure-time change in Live mode for no benefit.
   void InvalidateStackOnly();
   // Shared by OnBinning/OnFovSize: resets ROI to the new full frame and
   // resizes img_ accordingly, then calls InvalidateStack().
   void ApplyFrameSizeChange();

   // ---- precomputed-stack mode --------------------------------------------
   void StartStackGeneration();
   void StackGenerationWorker(long stackLength, unsigned fullW, unsigned fullH, sim::SimulationParams params,
                               long seed, sim::PsfGeneratorRequest psfRequest, sim::CellFieldSettings cellField,
                               double stageXUm, double stageYUm, double stageZUm, int modality);
   void CropFullFrameIntoImg(const std::vector<uint16_t>& fullFrame, unsigned fullW, unsigned fullH);

   // ---- live mode -----------------------------------------------------------
   void StartLiveProducer();
   void StartPsfPreload();
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

   // Precomputed-stack storage
   std::vector<std::vector<uint16_t>> stack_;
   unsigned stackFrameW_ = 0, stackFrameH_ = 0;
   // Frames of a precomputed stack (General_StackLength, default 1000). The
   // stack always loops (stackLoop_ is no longer a property).
   long stackLength_ = 1000;
   std::atomic<long> stackFramesGenerated_{0};
   std::atomic<bool> stackGenerating_{false};
   std::atomic<bool> stackReady_{false};
   long playbackIndex_ = 0;
   bool stackLoop_ = true;
   bool endOfStackReached_ = false;
   std::thread stackGenThread_;
   // The PSF kernel of the current PSFParam_ values, computed (or read from
   // the disk cache) in the background from Initialize() on, so the first
   // frame finds it in ComputePsfKernelCache's memo (StartPsfPreload).
   std::thread psfPreloadThread_;

   // Live-mode: always-running background producer thread + swap buffer,
   // decoupled from MM's Snap/Live/sequence pull cadence.
   std::thread liveProducerThread_;
   std::atomic<bool> liveProducerRun_{false};
   // Sequence acquisitions and the ZStage's z sequence (SharedStageState):
   // the acquisition epoch, whether one runs, whether it must skip frames
   // rendered before it started (an armed z sequence), and the epoch of the
   // frame in the front buffer (under frontFrameLock_).
   std::atomic<long> liveSeqEpoch_{0};
   std::atomic<bool> liveSeqCapture_{false};
   std::atomic<bool> liveSeqSkipStale_{false};
   long liveFrameEpoch_ = 0;
   // The liveConfigVersion_ the frame in the front buffer was rendered with
   // (under frontFrameLock_): a frame taken after a property change skips
   // frames that were already being rendered with the old settings.
   long liveFrameConfig_ = 0;
   // Drift restarts asked for (a Live/MDA sequence start): the producer
   // applies a restart at the start of its next frame (that frame has drift
   // 0; main's liveSeqStartTicks_ keeps frames started earlier out of the
   // sequence).
   std::atomic<long> liveDriftRestart_{0};
   // When the frame in the front buffer started (under frontFrameLock_): its
   // stage pose, focus, settings and illumination clocks were read after this.
   // A snap takes only a frame started after the snap was called, a sequence
   // acquisition only frames started after it began (liveSeqStartTicks_,
   // steady_clock ticks): a frame already in flight shows the pose and clocks
   // from before a stage move or the previous snap's light.
   sim::SharedStageState::Clock::time_point liveFrameStart_{};
   std::atomic<long long> liveSeqStartTicks_{0};
   // The light a live frame shone (under frontFrameLock_): its lit rect, its
   // exposure and the profile's dose weight. It goes into the illumination
   // history only when the frame is taken (a snap or a sequence acquisition:
   // the shutter is open); frames rendered while nothing acquires bleach
   // nothing.
   struct LitFrame
   {
      bool valid = false;
      double x0 = 0, y0 = 0, x1 = 0, y1 = 0, dtSec = 0;
      std::function<double(double, double)> weight;
   };
   LitFrame liveFrameLit_;
   // The z sequence the precomputed stack was made for (-1: none).
   std::atomic<long> stackZSeqVersion_{-1};
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
   std::mt19937_64 liveRng_;
   // Atomic because StartSequenceAcquisition() (main/MMCore thread) reads it
   // to compute liveDriftOriginFrame_ while LiveProducerLoop (producer
   // thread) increments it every tick.
   std::atomic<long> liveFrameCounter_{0};
   // Value of liveFrameCounter_ at the start of the current drift path:
   // LiveProducerLoop sums the drift steps of frames 1 ..
   // (liveFrameCounter_ - liveDriftOriginFrame_), so setting this to the
   // current liveFrameCounter_ resets drift to zero without
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
   // BackgroundPerSec and the dark current are rates (per second), converted
   // to frame-equivalent values with the camera's *current* MM Exposure.
   std::atomic<double> psfNa_{1.4};                    // objective numerical aperture
   std::atomic<double> pixelSizeNm_{100.0};
   std::atomic<double> backgroundPhotonsPerSec_{0.0};  // photons / pixel / s
   // Camera noise-chain defaults below (QuantumEfficiency, DarkCurrent,
   // Gain, ReadNoise) are the Photometrics Kinetix22 sCMOS, Sensitivity
   // (CMS) mode datasheet values -- the mode typically used for
   // photon-starved SMLM imaging -- per docs/dev/vectorial-psf-plan.md's "Noise
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
   // disabled (every pixel identical), matching the drift = 0
   // disabled-by-default convention used elsewhere. Photometrics doesn't
   // publish actual per-pixel variance for the Kinetix, so these two
   // defaults are estimates, not datasheet values -- see the plan doc.
   std::atomic<double> pixelGainStdPct_{0.5};
   std::atomic<double> pixelReadNoiseStdPct_{20.0};
   // Random-walk sample drift (sim::DriftSettings): RMS nm after 1 s, per
   // axis in x and y, and in z. Applies in both acquisition modes.
   std::atomic<double> driftXyNmPerSqrtSec_{0.0};
   std::atomic<double> driftZNmPerSqrtSec_{0.0};
   // Directed drift (DirectedDriftNumber; defaults in the constructor).
   std::atomic<double> directedDrift_[DD_COUNT];

   // ---- webSMLM parity round 2 -- every default below is "off", matching
   // webSMLM's realism=min, so a default movie is unchanged by them. ----
   // Excitation illumination profile (sim::IllumProfile, SMLMBackground.h),
   // peak-normalized; FWHM as percent of the FOV width.
   int illumProfile_ = static_cast<int>(sim::IllumProfile::Flat);
   std::atomic<double> illumFwhmPct_{60.0};
   // Sensor: sCMOS (the original chain) or EMCCD (sim::CameraNoiseParams).
   // Cic/BitDepth only matter for EMCCD; webSMLM's defaults. The EM gain is
   // not a value of its own: EmGain() = the preset's pre-amplifier
   // sensitivity / the gain (sim::EmGainFromGain).
   bool cameraEmccd_ = false;
   std::atomic<double> cicElectrons_{0.002};
   int bitDepth_ = 16;
   // Background fade-to-30%-floor time constant in seconds (0 = no fade).
   std::atomic<double> bgDecaySec_{0.0};
   // Sub-pixel PSF splat sampling mode -- Linear/Cubic remove the
   // 1/oversampling placement quantization; Nearest reproduces the original
   // box-average behavior exactly. See Simulation/PsfGeneratorBridge.h's
   // PsfInterpMode. Plain member, same convention as psfModel_.
   int psfInterp_ = static_cast<int>(sim::PsfInterpMode::Cubic);
   // Render diffraction-PSF frames (splat + noise) on the GPU when one is
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

   // CellField pattern (spec/PORT.md 9), indexed by CellFieldNumber; defaults
   // (set in the constructor) are the prototype's (spec/PORT.md 4.2), a sparse
   // 10% labelling (5.2) and no focus offset (focal plane = ZStage position
   // above the coverslip).
   std::atomic<double> cellField_[CF_COUNT];
   bool cellFieldPacking_ = true;
   // General_DiskCache: 0 Off, 1 Cells (packed blocks on disk), 2 CellsAndPsf.
   std::atomic<int> diskCacheMode_{1};

   // Imaging modality (General_ImagingModality: 0 Fluorescence, 1
   // BrightField) and the mean-field grid / BrightField numbers, indexed by
   // WideFieldNumber / BrightFieldNumber (defaults set in the constructor).
   std::atomic<int> modality_{0};

   // ---- issue 16: properties that are engine scope options ----
   struct OptionProp
   {
      std::string prop, option;
      int kind = 0;                    // 0 float, 1 integer, 2 named values (names[i] = value i + offset)
      std::vector<std::string> names;
      int offset = 0;
      double lo = 0, hi = 0;
   };
   std::vector<OptionProp> optionProps_;
   // A dye field property: the option prefix ("mt-dye" or "dye<N>") and the field.
   struct DyeFieldProp
   {
      std::string prop, prefix, field;
   };
   std::vector<DyeFieldProp> dyeFieldProps_;
   mutable std::mutex optionMutex_;
   std::map<std::string, double> option_;    // scope option -> value
   // Dye fields the user set (option "<prefix>.<field>" -> value); the others
   // read the library (so MM's value rounding never changes an untouched one).
   std::map<std::string, double> dyeEdited_;
   std::string lightPreset_ = "PAINT-640";   // Optics_Preset ("None" = the lasers as set)
   // Seconds of illumination per place of the sample (live frames and stacks
   // add to it; every fluorescence frame reads its dyes' clocks from it).
   sim::IlluminationHistory illumHistory_;
   std::mutex historyWorldMutex_;
   sim::CellFieldSettings historyWorld_;
   bool historyHaveWorld_ = false;
   int cameraPreset_ = 0;                     // CamParam_CameraPreset (index into the camera presets)
   int lastMtMode_ = -1;                      // the microtubules' effective mode at the last dye load
   std::atomic<double> wideFieldNum_[WF_COUNT];
   std::atomic<double> brightFieldNum_[BF_COUNT];

   // Diffraction PSF (Simulation/PsfGeneratorBridge.h: GibsonLanniZernike in
   // C++, RichardsWolf/GibsonLanni in the embedded PSFGenerator JVM) parameters. PsfModel gates which renderer is
   // used (Gaussian keeps the original analytic path); everything else
   // here feeds BuildPsfGeneratorRequest(). The model selector follows the
   // same plain-member convention as psfModel_ (read
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
   std::atomic<double> psfKernelHalfWidthNm_{7000.0};
   // JRE/JDK install root override for locating jvm.dll (empty =
   // auto-detect; see sim::FindJavaHome in PsfGeneratorBridge.cpp).
   // PSFGenerator itself and this project's bridge class are embedded in
   // this DLL -- no jar paths to configure.
   std::string psfGeneratorJavaHome_;

   // Z-stack range/step (PSF plan (docs/dev/vectorial-psf-plan.md) step 2), feeding req.nz/
   // req.zStepNm in BuildPsfGeneratorRequest(). Only matters with a
   // diffraction PsfModel. Random per-emitter Z spread (step 2) was reverted
   // in step 3 in favor of a real Z-stage device (InSiliScopeZStage.h/.cpp) --
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
   friend class CInSiliScopeCamera;

public:
   explicit SMLMSequenceThread(CInSiliScopeCamera* pCam);
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
   CInSiliScopeCamera* camera_ = nullptr;
   MM::MMTime startTime_;
   MMThreadLock stopLock_;
};
