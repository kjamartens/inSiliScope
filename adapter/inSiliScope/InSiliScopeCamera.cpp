///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeCamera.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   CInSiliScopeCamera: MM::Camera API, the camera's own
//                properties, and the sequence-acquisition thread. The frame
//                generation (live producer thread, precomputed stack, calls
//                into the simulation engine) lives in SMLMImageGeneration.cpp;
//                the settings' properties are the registry's
//                (Registry/PropertyTable.cpp).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"
#include "LiveClock.h"
#include "Simulation/CacheDir.h"
#include "Simulation/SharedStageState.h"
#include "Simulation/Timing.h"
#include "Simulation/Parallel.h"

#include "CameraImageMetadata.h"
#include "ModuleInterface.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

const char* g_CameraDeviceName = "Camera";

namespace {

// The pre-init sensor sizes (square). Larger ones render proportionally slower.
struct FovChoice
{
   const char* name;
   long px;
};
const FovChoice kFovChoices[] = {
   { "128x128", 128 },
   { "256x256", 256 },
   { "512x512 (slower)", 512 },
   { "1024x1024 (slow, ~4x the memory of 512)", 1024 },
};

const char* g_PropFovSize = "FovSize";
const char* g_PropActualFrameIntervalMs = "ActualFrameIntervalMs";
const char* g_AcqModePrecomputed = "Precomputed";
const char* g_AcqModeLive = "Live";

} // namespace

///////////////////////////////////////////////////////////////////////////////
// CInSiliScopeCamera implementation
///////////////////////////////////////////////////////////////////////////////

CInSiliScopeCamera::CInSiliScopeCamera()
{
   SetRegistryErrorTexts();
   thd_ = new SMLMSequenceThread(this);

   // Pre-init: the sensor's size (zoom in with MM's ROI).
   CreateStringProperty(g_PropFovSize, kFovChoices[1].name, false,
                        new CPropertyAction(this, &CInSiliScopeCamera::OnFovSize), true);
   for (const FovChoice& c : kFovChoices)
      AddAllowedValue(g_PropFovSize, c.name);
}

CInSiliScopeCamera::~CInSiliScopeCamera()
{
   StopSequenceAcquisition();
   StopLiveProducer();
   if (stackGenThread_.joinable())
      stackGenThread_.join();
   if (psfPreloadThread_.joinable())
      psfPreloadThread_.join();
   delete thd_;
}

void CInSiliScopeCamera::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_CameraDeviceName);
}

bool CInSiliScopeCamera::Busy()
{
   return stackGenerating_.load();
}

int CInSiliScopeCamera::Initialize()
{
   if (initialized_)
      return DEVICE_OK;
   const sim::TimingScope timing("init.camera");

   // The settings' rows (Registry/PropertyTable.cpp) for this session's Detail.
   int nRet = InitRegistry(g_CameraDeviceName);
   if (nRet != DEVICE_OK)
      return nRet;
   St().fovPx = cameraCCDXSize_;
   St().binning = binSize_;

   nRet = CreateStringProperty(MM::g_Keyword_Name, g_CameraDeviceName, true);
   if (nRet != DEVICE_OK)
      return nRet;
   nRet = CreateStringProperty(MM::g_Keyword_Description, "inSiliScope camera: a simulated sCMOS/EMCCD", true);
   if (nRet != DEVICE_OK)
      return nRet;
   nRet = CreateStringProperty(MM::g_Keyword_CameraName, "inSiliScope", true);
   if (nRet != DEVICE_OK)
      return nRet;
   nRet = CreateStringProperty(MM::g_Keyword_CameraID, "V2.0", true);
   if (nRet != DEVICE_OK)
      return nRet;

   // Binning
   CPropertyAction* pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnBinning);
   nRet = CreateIntegerProperty(MM::g_Keyword_Binning, 1, false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   AddAllowedValue(MM::g_Keyword_Binning, "1");
   AddAllowedValue(MM::g_Keyword_Binning, "2");
   AddAllowedValue(MM::g_Keyword_Binning, "4");
   AddAllowedValue(MM::g_Keyword_Binning, "8");

   // Pixel type: 16-bit only.
   nRet = CreateStringProperty(MM::g_Keyword_PixelType, "16-bit", true);
   if (nRet != DEVICE_OK)
      return nRet;

   // Exposure: the standard MM camera property and the only timing control.
   // It paces live frames, and every per-second rate (the dyes, the
   // background, the dark current) is converted to a frame with it.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnExposureProperty);
   nRet = CreateFloatProperty(MM::g_Keyword_Exposure, St().exposureMs.load(), false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   SetPropertyLimits(MM::g_Keyword_Exposure, 1.0, 10000.0);

   // The live loop's achieved frame interval (Advanced readout).
   if (Hub()->Shows(isc::Tier::Advanced))
   {
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnActualFrameIntervalMs);
      CreateFloatProperty(g_PropActualFrameIntervalMs, 0.0, true, pAct);
   }

   // Test rows (ISC_TEST=1 only): the precomputed, seeded stack that the
   // reproducibility checks and tools/adapter_pixel_hash.py use. A normal
   // session is always live.
   if (Hub()->TestRows())
   {
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnAcqMode);
      CreateStringProperty("Test_AcqMode", g_AcqModeLive, false, pAct);
      AddAllowedValue("Test_AcqMode", g_AcqModePrecomputed);
      AddAllowedValue("Test_AcqMode", g_AcqModeLive);
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnGenerateStack);
      CreateIntegerProperty("Test_GenerateStack", 0, false, pAct);
      SetPropertyLimits("Test_GenerateStack", 0, 1);
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnStackLength);
      CreateIntegerProperty("Test_StackLength", stackLength_, false, pAct);
      SetPropertyLimits("Test_StackLength", 1, 100000);
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnStackStatus);
      CreateStringProperty("Test_StackGenerationStatus", "Idle", true, pAct);
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnEndOfStackReached);
      CreateStringProperty("Test_EndOfStackReached", "No", true, pAct);
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnLiveRenderMs);
      CreateFloatProperty("Test_LiveRenderMs", 0.0, true, pAct);
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnLivePrefetchMs);
      CreateStringProperty("Test_LivePrefetchMs", "0/0", true, pAct);
      // The sample drift of the last frame taken ("x y z", nm).
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnDriftNm);
      CreateStringProperty("Test_DriftNm", "0 0 0", true, pAct);
      // The phase profile (Simulation/Timing.h; tools/bench_live.py --profile):
      // collecting On starts afresh, WriteTo writes the phases since then as
      // JSON to the given file and starts afresh again.
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnProfileCollect);
      CreateStringProperty("Test_ProfileCollect", "Off", false, pAct);
      AddAllowedValue("Test_ProfileCollect", "Off");
      AddAllowedValue("Test_ProfileCollect", "On");
      pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnProfileWriteTo);
      CreateStringProperty("Test_ProfileWriteTo", "", false, pAct);
   }

   nRet = UpdateStatus();
   if (nRet != DEVICE_OK)
      return nRet;

   roiX_ = 0;
   roiY_ = 0;
   roiXSize_ = FullWidth();
   roiYSize_ = FullHeight();
   img_.Resize(roiXSize_, roiYSize_, 2);
   img_.ResetPixels();

   liveFrameW_ = FullWidth();
   liveFrameH_ = FullHeight();
   frontFrame_.assign(static_cast<size_t>(liveFrameW_) * liveFrameH_, 0);
   backFrame_.assign(static_cast<size_t>(liveFrameW_) * liveFrameH_, 0);

   InvalidateStack();
   Hub()->SetInvalidateListener([this](isc::Invalidate what) { SettingsChanged(what); });

   initialized_ = true;

   // The diffraction PSF kernel is computed (or read from the disk cache,
   // Renderer DiskCache = CellsAndPsf) in the background from here on, so the
   // first frame finds it in the memo instead of waiting for it; a live loop
   // asking for the same kernel meanwhile waits for this thread rather than
   // computing it again.
   sim::SetPsfKernelDiskCacheDir(St().diskCacheMode.load() >= 2 ? sim::DefaultCacheDir() : std::string());
   // The JVM models (RichardsWolf, GibsonLanni) for the engine's dye states:
   // this camera's request at the state's emission wavelength.
   sim::SetScopePsfRequestHook([this](const sim::ScopeSpec&, double wavelengthNm, sim::PsfGeneratorRequest& req) {
      req = isc::BuildPsfGeneratorRequest(St());
      req.wavelengthNm = wavelengthNm;
      req.kernelHalfWidthPx =
         sim::PsfKernelHalfWidthPx(St().psfKernelHalfWidthNm.load(), req.pixelSizeNm, req.wavelengthNm, req.na);
      return true;
   });
   StartPsfPreload();
   if (acqMode_ == SMLM_MODE_LIVE)
      StartLiveProducer();

   return DEVICE_OK;
}

int CInSiliScopeCamera::OnFovSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   // Pre-init: set before Initialize (zoom in afterwards with MM's ROI).
   if (eAct == MM::BeforeGet)
   {
      for (const FovChoice& c : kFovChoices)
         if (c.px == cameraCCDXSize_)
            pProp->Set(c.name);
   }
   else if (eAct == MM::AfterSet)
   {
      if (initialized_)
         return DEVICE_CAN_NOT_SET_PROPERTY;
      std::string s;
      pProp->Get(s);
      for (const FovChoice& c : kFovChoices)
         if (s == c.name)
            cameraCCDXSize_ = cameraCCDYSize_ = c.px;
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnAcqMode(MM::PropertyBase* pProp, MM::ActionType eAct)
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

int CInSiliScopeCamera::Shutdown()
{
   StopSequenceAcquisition();
   StopLiveProducer();
   if (psfPreloadThread_.joinable())
      psfPreloadThread_.join();
   if (stackGenThread_.joinable())
      stackGenThread_.join();
   // The render threads are done: join the ParallelFor pool's workers (the
   // DLL may be unloaded next).
   sim::ParallelPoolShutdown();
   sim::SetScopePsfRequestHook(nullptr);
   if (Hub())
      Hub()->SetInvalidateListener(nullptr);
   ShutdownRegistry();
   initialized_ = false;
   return DEVICE_OK;
}

int CInSiliScopeCamera::SnapImage()
{
   const sim::TimingScope timing("mm.snap");
   const auto t0 = std::chrono::steady_clock::now();
   const double exp = GetExposure();

   GenerateNextFrameIntoImg(false);

   // A snap takes at least its exposure (a frame that rendered faster waits
   // out the rest, to the 0.1 ms: LiveClock.h).
   isc::PreciseWaiter().WaitUntil(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<double, std::milli>(exp)));
   return DEVICE_OK;
}

const unsigned char* CInSiliScopeCamera::GetImageBuffer()
{
   MMThreadGuard g(imgPixelsLock_);
   return const_cast<unsigned char*>(img_.GetPixels());
}

unsigned CInSiliScopeCamera::GetImageWidth() const { return img_.Width(); }
unsigned CInSiliScopeCamera::GetImageHeight() const { return img_.Height(); }
unsigned CInSiliScopeCamera::GetImageBytesPerPixel() const { return img_.Depth(); }
// The EMCCD path clips to its own bit depth (CamParam_BitDepth); tell MM so
// its display range matches. sCMOS stays 16-bit.
unsigned CInSiliScopeCamera::GetBitDepth() const
{
   return Hub() && St().emccd.load() ? static_cast<unsigned>(St().bitDepth.load()) : 16;
}
long CInSiliScopeCamera::GetImageBufferSize() const { return img_.Width() * img_.Height() * GetImageBytesPerPixel(); }

double CInSiliScopeCamera::GetExposure() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Exposure, buf);
   if (ret != DEVICE_OK)
      return 0.0;
   return atof(buf);
}

void CInSiliScopeCamera::SetExposure(double exp)
{
   SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exp));
   GetCoreCallback()->OnExposureChanged(this, exp);
}

int CInSiliScopeCamera::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
   MMThreadGuard g(imgPixelsLock_);
   unsigned fullW = FullWidth();
   unsigned fullH = FullHeight();
   if (xSize == 0 && ySize == 0)
   {
      roiX_ = 0;
      roiY_ = 0;
      roiXSize_ = fullW;
      roiYSize_ = fullH;
   }
   else
   {
      roiX_ = std::min(x, fullW > 0 ? fullW - 1 : 0);
      roiY_ = std::min(y, fullH > 0 ? fullH - 1 : 0);
      roiXSize_ = std::min(xSize, fullW - roiX_);
      roiYSize_ = std::min(ySize, fullH - roiY_);
   }
   img_.Resize(roiXSize_, roiYSize_, 2);
   return DEVICE_OK;
}

int CInSiliScopeCamera::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
   x = roiX_;
   y = roiY_;
   xSize = roiXSize_;
   ySize = roiYSize_;
   return DEVICE_OK;
}

int CInSiliScopeCamera::ClearROI()
{
   return SetROI(0, 0, 0, 0);
}

int CInSiliScopeCamera::GetBinning() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Binning, buf);
   if (ret != DEVICE_OK)
      return 1;
   return atoi(buf);
}

int CInSiliScopeCamera::SetBinning(int binF)
{
   return SetProperty(MM::g_Keyword_Binning, CDeviceUtils::ConvertToString(binF));
}

int CInSiliScopeCamera::StartSequenceAcquisition(double interval)
{
   return StartSequenceAcquisition(LONG_MAX, interval, false);
}

int CInSiliScopeCamera::StopSequenceAcquisition()
{
   if (thd_ && !thd_->IsStopped())
   {
      thd_->Stop();
      thd_->wait();
   }
   liveSeqCapture_ = false;
   liveSeqSkipStale_ = false;
   Stg().EndSequenceAcquisition();
   return DEVICE_OK;
}

int CInSiliScopeCamera::StartSequenceAcquisition(long numImages, double interval_ms, bool /*stopOnOverflow*/)
{
   if (IsCapturing())
      return DEVICE_CAMERA_BUSY_ACQUIRING;

   int ret = GetCoreCallback()->PrepareForAcq(this);
   if (ret != DEVICE_OK)
      return ret;

   sequenceStartTime_ = GetCurrentMMTime();
   imageCounter_ = 0;
   seqStartClock_ = std::chrono::steady_clock::now();
   seqIntervalMs_ = std::max(0.0, interval_ms);

   // The drift continues from where the sample is (it is the sample's, not
   // the acquisition's). An armed z sequence (hardware z stack) restarts at its first position;
   // the camera steps it one position per frame.
   const sim::SharedStageState::ZSequence zseq = Stg().GetZSequence();
   liveSeqEpoch_ = Stg().BeginSequenceAcquisition();
   // Live: the first frame is one started from here (the pose, focus and
   // settings of this moment); the producer counts a frame as the
   // acquisition's (the sample's time, its light) when capturing and started
   // after this instant (no start in between: never a frame counted early).
   liveSeqStartTicks_ = std::numeric_limits<long long>::max();
   liveSeqSkipStale_ = zseq.armed;
   liveSeqCapture_ = true;
   liveSeqStartTicks_ = sim::SharedStageState::Clock::now().time_since_epoch().count();
   if (acqMode_ != SMLM_MODE_LIVE)
   {
      // A precomputed stack made for another (or no) z sequence is remade
      // for this one: frame f at position f mod n.
      if (zseq.armed ? stackZSeqVersion_.load() != zseq.version : stackZSeqVersion_.load() != -1)
         InvalidateStackOnly();
      MMThreadGuard g(imgPixelsLock_);
      playbackIndex_ = 0;
      endOfStackReached_ = false;
   }

   thd_->Start(numImages, interval_ms);
   return DEVICE_OK;
}

int CInSiliScopeCamera::InsertImage()
{
   MM::MMTime timeStamp = GetCurrentMMTime();
   char label[MM::MaxStrLength];
   GetLabel(label);

   MM::CameraImageMetadata md;
   md.AddTag(MM::g_Keyword_Metadata_CameraLabel, label);
   md.AddTag(MM::g_Keyword_Elapsed_Time_ms,
             CDeviceUtils::ConvertToString((timeStamp - sequenceStartTime_).getMsec()));
   md.AddTag(MM::g_Keyword_Metadata_ROI_X, CDeviceUtils::ConvertToString(static_cast<long>(roiX_)));
   md.AddTag(MM::g_Keyword_Metadata_ROI_Y, CDeviceUtils::ConvertToString(static_cast<long>(roiY_)));

   imageCounter_++;

   char buf[MM::MaxStrLength];
   GetProperty(MM::g_Keyword_Binning, buf);
   md.AddTag(MM::g_Keyword_Binning, buf);

   MMThreadGuard g(imgPixelsLock_);
   const unsigned char* pI = img_.GetPixels();
   return GetCoreCallback()->InsertImage(this, pI, img_.Width(), img_.Height(), img_.Depth(), 1, md.Serialize());
}

int CInSiliScopeCamera::RunSequenceOnThread()
{
   // Live: the producer paces the frames (one per exposure, LiveProducerLoop),
   // so a frame is taken as soon as it is published; only an interval longer
   // than that holds frame i back to start + i x interval. Precomputed
   // playback (Test rows): frame i is inserted at start + (i + 1) x
   // max(exposure, interval). Absolute times, so waits do not add up.
   const bool live = acqMode_ == SMLM_MODE_LIVE;
   const double periodMs = std::max(seqIntervalMs_, live ? 0.0 : GetExposure());
   const long i = imageCounter_;
   isc::PreciseWaiter waiter;
   auto at = [&](long k) {
      return seqStartClock_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double, std::milli>(k * periodMs));
   };
   auto stopped = [this] { return thd_->IsStopped(); };
   if (live && periodMs > 0.0 && !waiter.WaitUntil(at(i), stopped))
      return DEVICE_OK;

   if (!GenerateNextFrameIntoImg(true))
   {
      // Aborted early: a stop was requested while we were waiting on
      // (auto-triggered) stack generation to finish. Skip this frame --
      // the thread's svc() loop will observe IsStopped() and exit.
      return DEVICE_OK;
   }

   if (!live && !waiter.WaitUntil(at(i + 1), stopped))
      return DEVICE_OK;
   const auto tInsert = std::chrono::steady_clock::now();
   const int ret = InsertImage();
   sim::TimingLog("mm.insert-image", std::chrono::duration<double>(std::chrono::steady_clock::now() - tInsert).count());
   return ret;
}

bool CInSiliScopeCamera::IsCapturing()
{
   return thd_ && !thd_->IsStopped();
}

void CInSiliScopeCamera::OnThreadExiting() throw()
{
   try
   {
      LogMessage("SMLM sequence acquisition thread exiting");
      liveSeqCapture_ = false;
      liveSeqSkipStale_ = false;
      Stg().EndSequenceAcquisition();
      if (GetCoreCallback())
         GetCoreCallback()->AcqFinished(this, 0);
   }
   catch (...)
   {
      LogMessage("Exception in OnThreadExiting", false);
   }
}

///////////////////////////////////////////////////////////////////////////////
// SMLMSequenceThread
///////////////////////////////////////////////////////////////////////////////

SMLMSequenceThread::SMLMSequenceThread(CInSiliScopeCamera* pCam) : camera_(pCam) {}
SMLMSequenceThread::~SMLMSequenceThread() {}

void SMLMSequenceThread::Stop()
{
   MMThreadGuard g(stopLock_);
   stop_ = true;
}

void SMLMSequenceThread::Start(long numImages, double intervalMs)
{
   MMThreadGuard g(stopLock_);
   numImages_ = numImages;
   intervalMs_ = intervalMs;
   imageCounter_ = 0;
   stop_ = false;
   activate();
   startTime_ = camera_->GetCurrentMMTime();
}

bool SMLMSequenceThread::IsStopped()
{
   MMThreadGuard g(stopLock_);
   return stop_;
}

int SMLMSequenceThread::svc()
{
   int ret = DEVICE_ERR;
   try
   {
      do
      {
         ret = camera_->RunSequenceOnThread();
      } while (DEVICE_OK == ret && !IsStopped() && imageCounter_++ < numImages_ - 1);
   }
   catch (...)
   {
      camera_->LogMessage("Exception in SMLM sequence thread", false);
   }
   {
      MMThreadGuard g(stopLock_);
      stop_ = true;
   }
   camera_->OnThreadExiting();
   return ret;
}
