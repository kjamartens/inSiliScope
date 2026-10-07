///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeCamera.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The inSiliScope camera, a peripheral of the hub
//                (InSiliScopeHub.h): renders what the simulated microscope
//                sees -- the mounted specimen through the objective, filters
//                and open light sources onto a simulated sCMOS/EMCCD -- live,
//                frame by frame at the stage's pose (the only mode in a
//                normal session), or as a seeded precomputed stack (the Test
//                rows, ISC_TEST=1). Its settings are the hub's
//                (Registry/SceneState.h); its own properties are the sensor
//                size (pre-init FovSize), exposure, binning and the readouts.
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
#include "InSiliScopeHub.h"
#include "Registry/RegistryDevice.h"
#include "Registry/SceneSettings.h"
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
#include <chrono>
#include <condition_variable>
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

class SMLMSequenceThread;

// The acquisition modes: Live (the only one in a normal session) renders on
// demand; Precomputed renders a seeded stack ahead and plays it back (the
// Test rows only, for reproducible checks).
enum SMLMAcqMode
{
   SMLM_MODE_PRECOMPUTED = 0,
   SMLM_MODE_LIVE = 1,
};

class CInSiliScopeCamera
   : public isc::RegistryDevice<CInSiliScopeCamera, CCameraBase<CInSiliScopeCamera>>
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

   // action interface: the camera's own properties (the settings' rows are
   // the registry's, Registry/PropertyTable.cpp)
   // ----------------
   int OnFovSize(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnExposureProperty(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnActualFrameIntervalMs(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnLiveRenderMs(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnLivePrefetchMs(MM::PropertyBase* pProp, MM::ActionType eAct);
   // Test rows (ISC_TEST=1): the precomputed stack.
   int OnAcqMode(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnGenerateStack(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackStatus(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnStackLength(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEndOfStackReached(MM::PropertyBase* pProp, MM::ActionType eAct);

   friend class SMLMSequenceThread;

private:
   // ---- the hub's settings and stage ----
   isc::SceneState& St() const { return Hub()->State(); }
   sim::SharedStageState& Stg() const { return Hub()->Stage(); }
   // ---- sizing / ROI / binning -------------------------------------------
   unsigned FullWidth() const { return static_cast<unsigned>(cameraCCDXSize_ / binSize_); }
   unsigned FullHeight() const { return static_cast<unsigned>(cameraCCDYSize_ / binSize_); }
   // The illumination field of a w x h frame (empty when flat), logged.
   sim::StackShapingFields ShapingFields(unsigned w, unsigned h) const;
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
   // BrightField precomputed stack: one image per distinct focus, times the
   // lamp flux, camera noise per frame; Z is read per batch of frames.
   void RenderBrightfieldStack(std::vector<std::vector<uint16_t>>& stack, long stackLength, unsigned w, unsigned h,
                               const sim::SimulationParams& params, const sim::CellFieldSettings& cellField,
                               double stageXUm, double stageYUm, const sim::PixelOffsetMap& offsetMap,
                               const sim::PixelGainMap& gainMap, const sim::PixelReadNoiseMap& readNoiseMap,
                               uint32_t noiseSeed, const std::vector<sim::DriftNm>& drift);
   // The calling thread's WideField GPU host (created once: gpu/tried are
   // the thread's), or nullptr for the CPU; sets the GPU status.
   sim::WidefieldGpuD3D11* WideFieldGpu(std::unique_ptr<sim::WidefieldGpuD3D11>& gpu, bool& tried);
   // Creates (if gpu is empty) and loads a GPU simulator with this
   // kernel/maps/background, when UseGpu is On and the frame can be
   // rendered on the GPU at all (diffraction kernel, not Fft placement).
   // Returns false -- the caller then renders on the CPU -- otherwise, or on
   // any D3D11 failure (logged once, and reported by GpuStatus).
   bool PrepareGpu(std::unique_ptr<sim::GpuSimulator>& gpu, const sim::PsfKernelCache& cache, unsigned w,
                   unsigned h, const sim::PixelOffsetMap& offsetMap, const sim::PixelGainMap& gainMap,
                   const sim::PixelReadNoiseMap& readNoiseMap, const sim::StackShapingFields& shaping,
                   const sim::SimulationParams& params);
   void SetGpuStatus(const std::string& s) { St().SetGpuStatus(s); }
   // A setting changed (the hub's listener): what it invalidates.
   void SettingsChanged(isc::Invalidate what);
   // Marks the precomputed stack stale (regenerated on next use) AND bumps
   // liveConfigVersion_, which LiveProducerLoop polls every tick to know
   // when to rebuild its cached offset map / emitter pattern.
   void InvalidateStack();
   // Marks the precomputed stack stale WITHOUT bumping liveConfigVersion_
   // (the exposure: the live loop reads it fresh every tick, and a rebuild
   // would recompute the PSF kernel for nothing).
   void InvalidateStackOnly();
   // Shared by OnBinning: resets ROI to the new full frame and resizes img_
   // accordingly, then calls InvalidateStack().
   void ApplyFrameSizeChange();

   // ---- precomputed-stack mode (Test rows) ----------------------------------
   void StartStackGeneration();
   void StackGenerationWorker(long stackLength, unsigned fullW, unsigned fullH, sim::SimulationParams params,
                               long seed, sim::CellFieldSettings cellField, double stageXUm, double stageYUm,
                               double stageZUm, int light);
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
   // thread), that wait is aborted early if thd_->IsStopped() becomes true;
   // returns false in that case (no frame was produced).
   bool GenerateNextFrameIntoImg(bool interruptible);

   // Device state
   bool initialized_ = false;
   ImgBuffer img_;
   unsigned roiX_ = 0, roiY_ = 0, roiXSize_ = 0, roiYSize_ = 0;
   long binSize_ = 1;
   // The sensor's size (pre-init FovSize: square).
   long cameraCCDXSize_ = 256;
   long cameraCCDYSize_ = 256;
   MMThreadLock imgPixelsLock_;

   int acqMode_ = SMLM_MODE_LIVE;

   // Precomputed-stack storage
   std::vector<std::vector<uint16_t>> stack_;
   unsigned stackFrameW_ = 0, stackFrameH_ = 0;
   // Frames of a precomputed stack (Test_StackLength, default 1000). The
   // stack always loops.
   long stackLength_ = 1000;
   std::atomic<long> stackFramesGenerated_{0};
   std::atomic<bool> stackGenerating_{false};
   std::atomic<bool> stackReady_{false};
   long playbackIndex_ = 0;
   bool stackLoop_ = true;
   bool endOfStackReached_ = false;
   std::thread stackGenThread_;
   // The PSF kernel of the current settings, computed (or read from the disk
   // cache) in the background from Initialize() on, so the first frame finds
   // it in ComputePsfKernelCache's memo (StartPsfPreload).
   std::thread psfPreloadThread_;

   // Live-mode: always-running background producer thread + swap buffer,
   // decoupled from MM's Snap/Live/sequence pull cadence.
   std::thread liveProducerThread_;
   std::atomic<bool> liveProducerRun_{false};
   // A publish wakes the consumers waiting for a new frame (no polling).
   std::mutex liveCvMutex_;
   std::condition_variable liveCv_;
   // A snap asks the producer to start its frame now instead of at the next
   // tick of the exposure schedule.
   std::atomic<bool> liveWakeNow_{false};
   // The sequence acquisition's start on the steady clock and its interval
   // (ms; 0 = as fast as the exposure allows): frame i is taken no earlier
   // than start + i x interval (precomputed playback: start + (i + 1) x
   // exposure).
   std::chrono::steady_clock::time_point seqStartClock_;
   double seqIntervalMs_ = 0.0;
   // Sequence acquisitions and the ZStage's z sequence (the hub's stage
   // state): the acquisition epoch, whether one runs, whether it must skip
   // frames rendered before it started (an armed z sequence), and the epoch of
   // the frame in the front buffer (under frontFrameLock_).
   std::atomic<long> liveSeqEpoch_{0};
   std::atomic<bool> liveSeqCapture_{false};
   std::atomic<bool> liveSeqSkipStale_{false};
   long liveFrameEpoch_ = 0;
   // The liveConfigVersion_ and light version the frame in the front buffer
   // was rendered with (under frontFrameLock_): a frame taken after a property
   // change or a shutter switch skips frames rendered with the old settings.
   long liveFrameConfig_ = 0;
   long liveFrameLight_ = 0;
   // Drift restarts asked for (a Live/MDA sequence start): the producer
   // applies a restart at the start of its next frame (that frame has drift 0).
   std::atomic<long> liveDriftRestart_{0};
   // When the frame in the front buffer started (under frontFrameLock_): its
   // stage pose, focus, settings and illumination clocks were read after this.
   // A snap takes only a frame started after the snap was called, a sequence
   // acquisition only frames started after it began (liveSeqStartTicks_,
   // steady_clock ticks).
   sim::SharedStageState::Clock::time_point liveFrameStart_{};
   std::atomic<long long> liveSeqStartTicks_{0};
   // The light a live frame shone (under frontFrameLock_): its lit rect, its
   // exposure and the profile's dose weight. It goes into the illumination
   // history only when the frame is taken (a snap or a sequence acquisition);
   // frames rendered while nothing acquires bleach nothing.
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
   // into frontFrame_; a consumer waits for it to advance (no duplicates).
   std::atomic<long> liveFrameSeq_{0};
   long lastConsumedLiveFrameSeq_ = -1;
   // lastConsumedLiveFrameSeq_ for the producer: during a hardware z stack it
   // renders the next position only once the last frame was taken (or
   // skipped), so no sequence position is lost.
   std::atomic<long> liveTakenSeq_{-1};
   // Rolling average of the wall-clock time between successive frame
   // publishes (ActualFrameIntervalMs).
   static constexpr int kFrameIntervalWindowSize = 10;
   double frameIntervalHistoryMs_[kFrameIntervalWindowSize] = {};
   int frameIntervalHistoryCount_ = 0;
   int frameIntervalHistoryPos_ = 0;
   MM::MMTime lastFramePublishTime_;
   std::atomic<double> actualFrameIntervalMs_{0.0};
   // Exponential average of the live producer's render time per frame (start
   // of the frame to its publish), ms (Test_LiveRenderMs, tools/bench_live.py).
   std::atomic<double> liveRenderMs_{0.0};
   // The longest prefetch in the producer's spare time since the last read,
   // and its budget, ms (Test_LivePrefetchMs: "longest/budget").
   std::atomic<double> livePrefetchMaxMs_{0.0}, livePrefetchBudgetMs_{0.0};
   std::mt19937_64 liveRng_;
   std::atomic<long> liveFrameCounter_{0};
   // Value of liveFrameCounter_ at the start of the current drift path
   // (reset in StartLiveProducer() and at every Live/MDA sequence start).
   std::atomic<long> liveDriftOriginFrame_{0};
   // Bumped by InvalidateStack(); LiveProducerLoop compares against its own
   // last-applied value each tick to know when to rebuild its cached state.
   std::atomic<long> liveConfigVersion_{0};

   SMLMSequenceThread* thd_ = nullptr;
   MM::MMTime sequenceStartTime_;
   long imageCounter_ = 0;

   // Seconds of illumination per place of the sample (live frames and stacks
   // add to it; every fluorescence frame reads its dyes' clocks from it).
   sim::IlluminationHistory illumHistory_;
   std::mutex historyWorldMutex_;
   sim::CellFieldSettings historyWorld_;
   bool historyHaveWorld_ = false;
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
