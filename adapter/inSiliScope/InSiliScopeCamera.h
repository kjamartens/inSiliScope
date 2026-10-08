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
#include "Simulation/Drift.h"
#include "Simulation/GpuSimD3D11.h"
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
   int OnDriftNm(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnProfileCollect(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnProfileWriteTo(MM::PropertyBase* pProp, MM::ActionType eAct);
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
   // Clears the history (and the sample's drift) when the world (seed, cells,
   // packing) changed.
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

   // ---- render-ahead (LiveAhead.cpp; spec/PERF_PASS.md phase 2) ------------
   // While a live/MDA sequence runs in an unchanged state, a helper thread
   // renders batches of the next frames with one movie (one setup and query
   // per batch, frames side by side on all cores, one GPU dispatch); the
   // producer hands them out one per exposure slot and drops them on any
   // change of state.
   // The camera's per-config statics a batch needs (copied by the producer
   // when its config version changes).
   struct LiveStatics
   {
      sim::PixelOffsetMap offsetMap;
      sim::PixelGainMap gainMap;
      sim::PixelReadNoiseMap readNoiseMap;
      sim::StackShapingFields shaping;
      long version = -1;
   };
   struct LiveBatchJob
   {
      long id = 0;
      sim::ScopeSpec spec;          // frames = the batch's length
      sim::ClockSnapshot clock;     // the dye clocks at its first frame
      sim::SimulationParams params;
      unsigned w = 0, h = 0;
      double zOffsetUm = 0.0;
      std::shared_ptr<const LiveStatics> statics;
      std::vector<double> fadeSec;  // per frame: the background fade's elapsed time (the lit clock)
      uint32_t epoch = 0;           // the rates its frames are lit with
      double decaySec = 0.0;
      uint32_t noiseSeed = 0, noiseBase = 0;
      bool useGpu = false;
      long firstSlot = 0;           // the live frame counter of its first frame
   };
   struct LiveBatch
   {
      long id = 0;
      bool ok = false;
      std::vector<std::vector<uint16_t>> frames;
      long zClamped = 0, zTotal = 0;
      std::string err;
      long firstSlot = 0;
      double ms = 0.0;              // how long it took to render
   };
   void StartLiveAhead();
   void StopLiveAhead();
   void LiveAheadLoop();
   // The producer's side: hand a job to the helper (it must be idle), take a
   // finished batch of job id (until: wait until it is done, at most until
   // then or until stop() says so; checked every 2 ms), drop all work.
   void SubmitLiveAhead(std::unique_ptr<LiveBatchJob> job);
   std::unique_ptr<LiveBatch> TakeLiveAhead(long id, const std::chrono::steady_clock::time_point* until,
                                            const std::function<bool()>& stop = nullptr);
   bool LiveAheadIdle();
   void CancelLiveAhead();
   std::thread liveAheadThread_;
   std::mutex liveAheadMutex_;
   std::condition_variable liveAheadCv_;
   std::unique_ptr<LiveBatchJob> liveAheadJob_;   // submitted, not started
   std::unique_ptr<LiveBatch> liveAheadDone_;     // finished, not collected
   bool liveAheadBusy_ = false;                   // a job is rendering
   bool liveAheadRun_ = false;
   std::atomic<long> liveAheadCancel_{0};         // jobs with an id <= this stop early
   long liveAheadLastId_ = 0;                     // the last job submitted
   // The consumer takes a frame and adds its light to the history under this
   // lock; the producer reads the history and whether the front frame was
   // taken under it too (a batch's clocks: the history plus the frames not
   // yet taken).
   std::mutex liveTakeMutex_;
   // The live GPU hosts, shared by the producer and the render-ahead helper
   // and kept across live starts (creating a Direct3D 11 device, compiling
   // its shaders and the self-check take seconds). Whoever renders with them
   // holds `m`; the producer only try-locks it (when the helper has it, the
   // producer's frame renders on the CPU instead of waiting).
   struct LiveGpu
   {
      std::mutex m;
      std::unique_ptr<sim::GpuSimulator> splat;
      uint64_t kernelSerial = 0;   // the kernel loaded (0: none)
      double background = -1.0;    // and its background
      long statics = -1;           // the config version of the camera maps loaded
      long failedAt = -1;          // the config version the splat failed at
      std::unique_ptr<sim::WidefieldGpuD3D11> wf;
      bool wfTried = false;
   };
   LiveGpu liveGpu_;
   // Prepares liveGpu_.splat (hold liveGpu_.m) for this plan and config
   // version; false: render on the CPU.
   bool PrepareLiveGpu(const sim::FluorescenceSimplePlan& plan, unsigned w, unsigned h, const LiveStatics& st,
                       const sim::SimulationParams& params);

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
   // The sample's drift (Simulation/Drift.h): one walker for the session,
   // stepped by the producer -- every frame while SampleHolder.TimeWhileIdle
   // is Running, else only the frames of an acquisition -- so it continues
   // across Live stop/start; reset with the world (SyncHistoryWorld). The
   // precomputed stack keeps its own seeded path from 0.
   sim::DriftWalker driftWalker_;
   std::atomic<long> driftWorldResets_{0};
   long driftResetApplied_ = -1;
   // The drift of the last frame taken (Camera.Test_DriftNm), nm.
   std::atomic<double> takenDriftNm_[3] = {};
   // When the frame in the front buffer started (under frontFrameLock_): its
   // stage pose, focus, settings and illumination clocks were read after this.
   // A snap takes only a frame started after the snap was called, a sequence
   // acquisition only frames started after it began (liveSeqStartTicks_,
   // steady_clock ticks).
   sim::SharedStageState::Clock::time_point liveFrameStart_{};
   // When the front frame was published (its age when taken: mm.frame-age).
   sim::SharedStageState::Clock::time_point liveFramePublished_{};
   std::atomic<long long> liveSeqStartTicks_{0};
   // The light a live frame shone (under frontFrameLock_): its lit rect (the
   // drifted FOV: the light follows the sample), its exposure, the profile's
   // dose weight and the epoch (the rates it was lit with). It goes into the
   // illumination history when the frame is published if it lights the
   // sample then (an acquisition's frame, or TimeWhileIdle Running with the
   // lasers' shutter opened on purpose; light reaching the sample), else when
   // a snap takes it (counted: already in). Also the frame's drift.
   struct LitFrame
   {
      bool valid = false, counted = false;
      double x0 = 0, y0 = 0, x1 = 0, y1 = 0, dtSec = 0;
      std::function<double(double, double)> weight;
      uint32_t epoch = 0;
      double driftNm[3] = { 0, 0, 0 };
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
   std::atomic<long> liveFrameCounter_{0};
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
