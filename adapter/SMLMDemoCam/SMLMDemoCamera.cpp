///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMDemoCamera.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   CSMLMDemoCamera: MM::Camera API, property handlers, and the
//                sequence-acquisition thread. The actual frame generation
//                (precomputed-stack management, live producer thread, and
//                calls into the simulation engine) lives in
//                SMLMImageGeneration.cpp.
//
// LICENSE:       BSD (see license.txt)

#include "SMLMDemoCamera.h"

#include "CameraImageMetadata.h"
#include "ModuleInterface.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sstream>

const char* g_SMLMCameraDeviceName = "SMLMDemoCam";

const char* g_PropAcqMode = "AcqMode";
const char* g_PropPattern = "Pattern";
const char* g_PropCustomPointsFile = "CustomPointsFile";
const char* g_PropResolutionSpacingsNm = "ResolutionSpacingsNm";
const char* g_PropFovSize = "FovSize";
const char* g_PropStackLength = "StackLength";
const char* g_PropStackLoop = "StackLoop";
const char* g_PropGenerateStack = "GenerateStack";
const char* g_PropStackStatus = "StackGenerationStatus";
const char* g_PropEndOfStack = "EndOfStackReached";
const char* g_PropEmitterDensityPerSec = "EmitterDensityPerSec";
const char* g_PropPhotonsPerSecond = "PhotonsPerSecond";
const char* g_PropOnLifetimeSec = "OnLifetimeSec";
const char* g_PropPsfWavelengthNm = "PsfEmissionWavelengthNm";
const char* g_PropPsfNa = "PsfNa";
const char* g_PropPixelSize = "PixelSizeNm";
const char* g_PropBackgroundPerSec = "BackgroundPhotonsPerSec";
const char* g_PropGain = "CameraGainPhotonsPerADU";
const char* g_PropOffset = "CameraOffsetADU";
const char* g_PropOffsetStd = "CameraOffsetStdADU";
const char* g_PropReadNoise = "ReadNoiseElectrons";
const char* g_PropDriftNmPerSec = "DriftNmPerSec";
const char* g_PropRandomSeed = "RandomSeed";
const char* g_PropActualFrameIntervalMs = "ActualFrameIntervalMs";
const char* g_PropPsfModel = "PsfModel";
const char* g_PropPsfImmersionIndex = "PsfImmersionIndex";
const char* g_PropPsfOversampling = "PsfOversampling";
const char* g_PropPsfKernelHalfWidthPx = "PsfKernelHalfWidthPx";
const char* g_PropPsfGeneratorJavaHome = "PsfGeneratorJavaHome";
const char* g_PropPsfZRangeUm = "PsfZRangeUm";
const char* g_PropPsfZStepUm = "PsfZStepUm";
const char* g_PropPsfZSpreadStdNm = "PsfZSpreadStdNm";
const char* g_PropPsfSampleIndex = "PsfSampleIndex";
const char* g_PropPsfWorkingDistanceUm = "PsfWorkingDistanceUm";
const char* g_PropPsfSampleDepthNm = "PsfSampleDepthNm";

const char* g_PsfModelGaussian = "Gaussian";
const char* g_PsfModelRichardsWolf = "RichardsWolf";
const char* g_PsfModelGibsonLanni = "GibsonLanni";

const char* g_AcqModePrecomputed = "Precomputed";
const char* g_AcqModeLive = "Live";

const char* g_PatternCircle = "Circle";
const char* g_PatternLines = "Lines";
const char* g_PatternGrid = "Grid";
const char* g_PatternRandom = "Random";
const char* g_PatternCustom = "CustomPoints";
const char* g_PatternSpiral = "Spiral";
const char* g_PatternStar = "Star";
const char* g_PatternHeart = "Heart";
const char* g_PatternResolutionTarget = "ResolutionTarget";

const char* g_Fov128 = "128x128";
const char* g_Fov256 = "256x256";
const char* g_Fov512 = "512x512";

namespace {
const char* g_YesNo[] = {"On", "Off"};
} // namespace

///////////////////////////////////////////////////////////////////////////////
// CSMLMDemoCamera implementation
///////////////////////////////////////////////////////////////////////////////

CSMLMDemoCamera::CSMLMDemoCamera()
{
   InitializeDefaultErrorMessages();
   thd_ = new SMLMSequenceThread(this);

   // Pre-init property: must exist before Initialize() finishes. FovSize is
   // deliberately NOT pre-init -- unlike RandomSeed, it's a regular,
   // live-changeable property (see OnFovSize/ApplyFrameSizeChange), same as
   // Binning.
   CreateIntegerProperty(g_PropRandomSeed, randomSeed_, false,
                          new CPropertyAction(this, &CSMLMDemoCamera::OnRandomSeed), true);
}

CSMLMDemoCamera::~CSMLMDemoCamera()
{
   StopSequenceAcquisition();
   StopLiveProducer();
   if (stackGenThread_.joinable())
      stackGenThread_.join();
   delete thd_;
}

void CSMLMDemoCamera::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_SMLMCameraDeviceName);
}

bool CSMLMDemoCamera::Busy()
{
   return stackGenerating_.load();
}

int CSMLMDemoCamera::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   int nRet = CreateStringProperty(MM::g_Keyword_Name, g_SMLMCameraDeviceName, true);
   if (nRet != DEVICE_OK)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_Description, "Synthetic SMLM demo camera", true);
   if (nRet != DEVICE_OK)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_CameraName, "SMLMDemoCam", true);
   if (nRet != DEVICE_OK)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_CameraID, "V1.0", true);
   if (nRet != DEVICE_OK)
      return nRet;

   // FovSize: a regular, live-changeable property (not pre-init) -- can be
   // changed after the device has been added/initialized, same as Binning.
   CPropertyAction* pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnFovSize);
   nRet = CreateStringProperty(g_PropFovSize, g_Fov256, false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   AddAllowedValue(g_PropFovSize, g_Fov128);
   AddAllowedValue(g_PropFovSize, g_Fov256);
   AddAllowedValue(g_PropFovSize, g_Fov512);

   // Binning
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBinning);
   nRet = CreateIntegerProperty(MM::g_Keyword_Binning, 1, false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   AddAllowedValue(MM::g_Keyword_Binning, "1");
   AddAllowedValue(MM::g_Keyword_Binning, "2");
   AddAllowedValue(MM::g_Keyword_Binning, "4");
   AddAllowedValue(MM::g_Keyword_Binning, "8");

   // Pixel type: 16-bit only for v1.
   nRet = CreateStringProperty(MM::g_Keyword_PixelType, "16-bit", true);
   if (nRet != DEVICE_OK)
      return nRet;

   // Exposure: the standard MM camera property, and the *only* exposure/
   // timing control this device exposes -- no separate device-specific
   // exposure property. It drives frame pacing (Live), simulated exposure
   // timing (Snap in Precomputed mode), and -- via SnapshotParams() --
   // converts every rate-based simulation parameter (EmitterDensityPerSec,
   // OnLifetimeSec, PhotonsPerSecond, BackgroundPhotonsPerSec) into the
   // frame-equivalent quantity for whatever this is currently set to.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnExposureProperty);
   nRet = CreateFloatProperty(MM::g_Keyword_Exposure, 50.0, false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   SetPropertyLimits(MM::g_Keyword_Exposure, 1.0, 10000.0);

   // Acquisition mode. Default is Live -- continuous on-the-fly simulation,
   // no precomputed-stack generation delay before the first frame appears.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnAcqMode);
   CreateStringProperty(g_PropAcqMode, g_AcqModeLive, false, pAct);
   AddAllowedValue(g_PropAcqMode, g_AcqModePrecomputed);
   AddAllowedValue(g_PropAcqMode, g_AcqModeLive);

   // Pattern
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPattern);
   CreateStringProperty(g_PropPattern, g_PatternCircle, false, pAct);
   AddAllowedValue(g_PropPattern, g_PatternCircle);
   AddAllowedValue(g_PropPattern, g_PatternLines);
   AddAllowedValue(g_PropPattern, g_PatternGrid);
   AddAllowedValue(g_PropPattern, g_PatternRandom);
   AddAllowedValue(g_PropPattern, g_PatternCustom);
   AddAllowedValue(g_PropPattern, g_PatternSpiral);
   AddAllowedValue(g_PropPattern, g_PatternStar);
   AddAllowedValue(g_PropPattern, g_PatternHeart);
   AddAllowedValue(g_PropPattern, g_PatternResolutionTarget);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCustomPointsFile);
   CreateStringProperty(g_PropCustomPointsFile, "", false, pAct);

   // Ring/scale-step/spiral-arc gap (Circle/Spiral/Star/Heart) and per-cell
   // line spacing (ResolutionTarget), easiest to hardest, nm, comma-
   // separated. Any positive count of values is accepted; the smallest
   // value drives the finest ring/cell, the largest the coarsest.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnResolutionSpacingsNm);
   CreateStringProperty(g_PropResolutionSpacingsNm,
                         sim::FormatResolutionSpacingsNm(resolutionSpacingsNm_).c_str(), false, pAct);

   // Precomputed-stack properties
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnStackLength);
   CreateIntegerProperty(g_PropStackLength, stackLength_, false, pAct);
   SetPropertyLimits(g_PropStackLength, 10, 20000);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnStackLoop);
   CreateStringProperty(g_PropStackLoop, g_YesNo[0], false, pAct);
   AddAllowedValue(g_PropStackLoop, g_YesNo[0]);
   AddAllowedValue(g_PropStackLoop, g_YesNo[1]);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnGenerateStack);
   CreateIntegerProperty(g_PropGenerateStack, 0, false, pAct);
   SetPropertyLimits(g_PropGenerateStack, 0, 1);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnStackStatus);
   CreateStringProperty(g_PropStackStatus, "Idle", true, pAct);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnEndOfStackReached);
   CreateStringProperty(g_PropEndOfStack, "No", true, pAct);

   // Simulation parameters. EmitterDensityPerSec/PhotonsPerSecond/
   // OnLifetimeSec/BackgroundPhotonsPerSec are rates (per second) that scale
   // automatically with the standard Exposure property -- see
   // SnapshotParams() in SMLMImageGeneration.cpp.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnEmitterDensityPerSec);
   CreateFloatProperty(g_PropEmitterDensityPerSec, emitterDensityPerSec_.load(), false, pAct);
   SetPropertyLimits(g_PropEmitterDensityPerSec, 0.1, 500.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPhotonsPerSecond);
   CreateFloatProperty(g_PropPhotonsPerSecond, photonsPerSecond_.load(), false, pAct);
   SetPropertyLimits(g_PropPhotonsPerSecond, 1000.0, 2000000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnOnLifetimeSec);
   CreateFloatProperty(g_PropOnLifetimeSec, onLifetimeSec_.load(), false, pAct);
   SetPropertyLimits(g_PropOnLifetimeSec, 0.001, 10.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfWavelengthNm);
   CreateFloatProperty(g_PropPsfWavelengthNm, psfWavelengthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfWavelengthNm, 400.0, 800.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfNa);
   CreateFloatProperty(g_PropPsfNa, psfNa_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfNa, 0.5, 1.49);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPixelSizeNm);
   CreateFloatProperty(g_PropPixelSize, pixelSizeNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPixelSize, 10.0, 1000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBackgroundPerSec);
   CreateFloatProperty(g_PropBackgroundPerSec, backgroundPhotonsPerSec_.load(), false, pAct);
   SetPropertyLimits(g_PropBackgroundPerSec, 0.0, 200000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCameraGain);
   CreateFloatProperty(g_PropGain, gainPhotonsPerAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropGain, 0.1, 100.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCameraOffset);
   CreateFloatProperty(g_PropOffset, offsetAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropOffset, 0.0, 10000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnOffsetStd);
   CreateFloatProperty(g_PropOffsetStd, offsetStdAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropOffsetStd, 0.0, 500.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnReadNoise);
   CreateFloatProperty(g_PropReadNoise, readNoiseElectrons_.load(), false, pAct);
   SetPropertyLimits(g_PropReadNoise, 0.0, 100.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnDriftNmPerSec);
   CreateFloatProperty(g_PropDriftNmPerSec, driftNmPerSecX_.load(), false, pAct);
   SetPropertyLimits(g_PropDriftNmPerSec, 0.0, 20000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnActualFrameIntervalMs);
   CreateFloatProperty(g_PropActualFrameIntervalMs, 0.0, true, pAct);

   // Vectorial PSF (embedded PSFGenerator JVM bridge). Default model is
   // GibsonLanni -- see psfModel_'s own initializer in SMLMDemoCamera.h;
   // this string just needs to agree with it.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfModel);
   CreateStringProperty(g_PropPsfModel, g_PsfModelGibsonLanni, false, pAct);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGaussian);
   AddAllowedValue(g_PropPsfModel, g_PsfModelRichardsWolf);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGibsonLanni);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfImmersionIndex);
   CreateFloatProperty(g_PropPsfImmersionIndex, psfImmersionIndex_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfImmersionIndex, 1.0, 2.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfOversampling);
   CreateIntegerProperty(g_PropPsfOversampling, psfOversampling_, false, pAct);
   SetPropertyLimits(g_PropPsfOversampling, 1, 16);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfKernelHalfWidthPx);
   CreateIntegerProperty(g_PropPsfKernelHalfWidthPx, psfKernelHalfWidthPx_, false, pAct);
   SetPropertyLimits(g_PropPsfKernelHalfWidthPx, 2, 32);

   // PSFGenerator itself (and this project's bridge class) are embedded in
   // this DLL -- nothing to point at except, optionally, a specific JRE/JDK
   // install to supply jvm.dll. Left empty (the default), the JVM
   // auto-detects one (JAVA_HOME, then common install locations -- see
   // sim::FindJavaHome in PsfGeneratorBridge.cpp).
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfGeneratorJavaHome);
   CreateStringProperty(g_PropPsfGeneratorJavaHome, psfGeneratorJavaHome_.c_str(), false, pAct);

   // Z-stack + random per-emitter Z spread (vectorial PSF plan step 2).
   // PsfZSpreadStdNm defaults to 0 (disabled): every emitter stays in-focus,
   // reproducing step 1's behavior exactly, until deliberately turned on.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZRangeUm);
   CreateFloatProperty(g_PropPsfZRangeUm, psfZRangeUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZRangeUm, 0.1, 10.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZStepUm);
   CreateFloatProperty(g_PropPsfZStepUm, psfZStepUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZStepUm, 0.01, 1.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZSpreadStdNm);
   CreateFloatProperty(g_PropPsfZSpreadStdNm, psfZSpreadStdNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZSpreadStdNm, 0.0, 2000.0);

   // GibsonLanni-only parameters (ignored by RichardsWolf); see the comments
   // on psfSampleIndex_/psfWorkingDistanceUm_/psfSampleDepthNm_ in
   // SMLMDemoCamera.h for why these particular defaults reproduce the
   // no-mismatch/in-focus behavior this bridge used to hardcode.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfSampleIndex);
   CreateFloatProperty(g_PropPsfSampleIndex, psfSampleIndex_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfSampleIndex, 1.0, 2.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfWorkingDistanceUm);
   CreateFloatProperty(g_PropPsfWorkingDistanceUm, psfWorkingDistanceUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfWorkingDistanceUm, 0.0, 9999.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfSampleDepthNm);
   CreateFloatProperty(g_PropPsfSampleDepthNm, psfSampleDepthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfSampleDepthNm, -100000.0, 100000.0);

   nRet = UpdateStatus();
   if (nRet != DEVICE_OK)
      return nRet;

   rng_.seed(static_cast<uint64_t>(randomSeed_));
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

   initialized_ = true;

   // Precomputed-stack generation is intentionally NOT started here -- it's
   // lazily auto-triggered on first Snap/Live/sequence use (see
   // GenerateNextFrameIntoImg() in SMLMImageGeneration.cpp). Live mode (the
   // default) does need its always-on producer thread running from the
   // start, since unlike stack generation it isn't a one-shot job with a
   // natural "trigger" point.
   if (acqMode_ == SMLM_MODE_LIVE)
      StartLiveProducer();

   return DEVICE_OK;
}

int CSMLMDemoCamera::Shutdown()
{
   StopSequenceAcquisition();
   StopLiveProducer();
   if (stackGenThread_.joinable())
      stackGenThread_.join();
   initialized_ = false;
   return DEVICE_OK;
}

int CSMLMDemoCamera::SnapImage()
{
   MM::MMTime startTime = GetCurrentMMTime();
   double exp = GetExposure();

   GenerateNextFrameIntoImg(false);

   MM::MMTime s0(0, 0);
   if (s0 < startTime)
   {
      while (exp > (GetCurrentMMTime() - startTime).getMsec())
         CDeviceUtils::SleepMs(1);
   }
   return DEVICE_OK;
}

const unsigned char* CSMLMDemoCamera::GetImageBuffer()
{
   MMThreadGuard g(imgPixelsLock_);
   return const_cast<unsigned char*>(img_.GetPixels());
}

unsigned CSMLMDemoCamera::GetImageWidth() const { return img_.Width(); }
unsigned CSMLMDemoCamera::GetImageHeight() const { return img_.Height(); }
unsigned CSMLMDemoCamera::GetImageBytesPerPixel() const { return img_.Depth(); }
unsigned CSMLMDemoCamera::GetBitDepth() const { return 16; }
long CSMLMDemoCamera::GetImageBufferSize() const { return img_.Width() * img_.Height() * GetImageBytesPerPixel(); }

double CSMLMDemoCamera::GetExposure() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Exposure, buf);
   if (ret != DEVICE_OK)
      return 0.0;
   return atof(buf);
}

void CSMLMDemoCamera::SetExposure(double exp)
{
   SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exp));
   GetCoreCallback()->OnExposureChanged(this, exp);
}

int CSMLMDemoCamera::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
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

int CSMLMDemoCamera::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
   x = roiX_;
   y = roiY_;
   xSize = roiXSize_;
   ySize = roiYSize_;
   return DEVICE_OK;
}

int CSMLMDemoCamera::ClearROI()
{
   return SetROI(0, 0, 0, 0);
}

int CSMLMDemoCamera::GetBinning() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Binning, buf);
   if (ret != DEVICE_OK)
      return 1;
   return atoi(buf);
}

int CSMLMDemoCamera::SetBinning(int binF)
{
   return SetProperty(MM::g_Keyword_Binning, CDeviceUtils::ConvertToString(binF));
}

int CSMLMDemoCamera::StartSequenceAcquisition(double interval)
{
   return StartSequenceAcquisition(LONG_MAX, interval, false);
}

int CSMLMDemoCamera::StopSequenceAcquisition()
{
   if (thd_ && !thd_->IsStopped())
   {
      thd_->Stop();
      thd_->wait();
   }
   return DEVICE_OK;
}

int CSMLMDemoCamera::StartSequenceAcquisition(long numImages, double interval_ms, bool /*stopOnOverflow*/)
{
   if (IsCapturing())
      return DEVICE_CAMERA_BUSY_ACQUIRING;

   int ret = GetCoreCallback()->PrepareForAcq(this);
   if (ret != DEVICE_OK)
      return ret;

   sequenceStartTime_ = GetCurrentMMTime();
   imageCounter_ = 0;

   // A fresh Live/MDA acquisition restarts the drift ramp from zero rather
   // than continuing wherever the previous acquisition left off.
   if (acqMode_ == SMLM_MODE_LIVE)
   {
      liveDriftOriginFrame_ = liveFrameCounter_.load();
   }
   else
   {
      MMThreadGuard g(imgPixelsLock_);
      playbackIndex_ = 0;
      endOfStackReached_ = false;
   }

   thd_->Start(numImages, interval_ms);
   return DEVICE_OK;
}

int CSMLMDemoCamera::InsertImage()
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

int CSMLMDemoCamera::RunSequenceOnThread()
{
   MM::MMTime startTime = GetCurrentMMTime();
   double exposure = GetExposure();

   if (!GenerateNextFrameIntoImg(true))
   {
      // Aborted early: a stop was requested while we were waiting on
      // (auto-triggered) stack generation to finish. Skip this frame --
      // the thread's svc() loop will observe IsStopped() and exit.
      return DEVICE_OK;
   }

   while ((GetCurrentMMTime() - startTime).getMsec() < exposure)
   {
      if (thd_->IsStopped())
         break;
      CDeviceUtils::SleepMs(1);
   }

   return InsertImage();
}

bool CSMLMDemoCamera::IsCapturing()
{
   return thd_ && !thd_->IsStopped();
}

void CSMLMDemoCamera::OnThreadExiting() throw()
{
   try
   {
      LogMessage("SMLM sequence acquisition thread exiting");
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

SMLMSequenceThread::SMLMSequenceThread(CSMLMDemoCamera* pCam) : camera_(pCam) {}
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
