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

const char* g_PropAcqMode = "General_AcqMode";
const char* g_PropPattern = "SimType_Pattern";
const char* g_PropCustomPointsFile = "SimType_CustomPointsFile";
const char* g_PropResolutionSpacingsNm = "SimType_ResolutionSpacingsNm";
const char* g_PropFovSize = "General_FovSize";
const char* g_PropGenerateStack = "General_GenerateStack";
const char* g_PropStackStatus = "General_StackGenerationStatus";
const char* g_PropEndOfStack = "General_EndOfStackReached";
const char* g_PropEmitterDensityPerSec = "General_EmitterDensityPerSec";
const char* g_PropPhotonsPerSecond = "FluoParam_PhotonsPerSecond";
const char* g_PropOnLifetimeSec = "FluoParam_OnLifetimeSec";
const char* g_PropPsfWavelengthNm = "PSFParam_PsfEmissionWavelengthNm";
const char* g_PropPsfNa = "PSFParam_PsfNa";
const char* g_PropPixelSize = "General_PixelSizeNm";
const char* g_PropBackgroundPerSec = "Background_BackgroundPhotonsPerSec";
const char* g_PropQuantumEfficiency = "CamParam_QuantumEfficiency";
const char* g_PropDarkCurrentPerSec = "CamParam_DarkCurrentElectronsPerSec";
const char* g_PropGain = "CamParam_GainPhotonsPerADU";
const char* g_PropOffset = "CamParam_OffsetADU";
const char* g_PropOffsetStd = "CamParam_OffsetStdADU";
const char* g_PropReadNoise = "CamParam_ReadNoiseElectrons";
const char* g_PropPixelGainStdPct = "CamParam_GainStdPctPerPixel";
const char* g_PropPixelReadNoiseStdPct = "CamParam_ReadNoiseStdPctPerPixel";
const char* g_PropDriftNmPerSec = "SimType_DriftNmPerSec";
const char* g_PropRandomSeed = "SimType_RandomSeed";
const char* g_PropActualFrameIntervalMs = "General_ActualFrameIntervalMs";
const char* g_PropPsfModel = "PSFParam_PsfModel";
const char* g_PropPsfImmersionIndex = "PSFParam_PsfImmersionIndex";
const char* g_PropPsfOversampling = "PSFParam_PsfOversampling";
const char* g_PropPsfKernelHalfWidthNm = "PSFParam_PsfKernelHalfWidthNm";
const char* g_PropPsfGeneratorJavaHome = "PSFParam_PsfGeneratorJavaHome";
const char* g_PropPsfZRangeUm = "PSFParam_PsfZRangeUm";
const char* g_PropPsfZStepUm = "PSFParam_PsfZStepUm";
const char* g_PropPsfSampleIndex = "PSFParam_PsfSampleIndex";
const char* g_PropPsfWorkingDistanceUm = "PSFParam_PsfWorkingDistanceUm";
const char* g_PropPsfSampleDepthNm = "PSFParam_PsfSampleDepthNm";
const char* g_PropPsfZernikeCoefficients = "PSFParam_PsfZernikeCoefficients";
const char* g_PropPsfZernikePreset = "PSFParam_PsfZernikePreset";

const char* g_PropLabelingEfficiencyPct = "General_LabelingEfficiencyPct";
const char* g_PropStructureZRangeNm = "SimType_StructureZRangeNm";
const char* g_PropStructureSizeNm = "SimType_StructureSizeNm";
const char* g_PropNupRadiusNm = "SimType_NupRadiusNm";
const char* g_PropNupCornerSpreadNm = "SimType_NupCornerSpreadNm";
const char* g_PropNupRingSeparationNm = "SimType_NupRingSeparationNm";
const char* g_PropNupLinkerMinNm = "SimType_NupLinkerMinNm";
const char* g_PropNupLinkerMaxNm = "SimType_NupLinkerMaxNm";
const char* g_PropNupMembraneType = "SimType_NupMembraneType";
const char* g_PropNupCount = "SimType_NupCount";
const char* g_PropNupMinSpacingNm = "SimType_NupMinSpacingNm";
const char* g_PropNupCurvatureNm = "SimType_NupCurvatureNm";

const char* g_NupMembraneTopDown = "TopDown";
const char* g_NupMembraneSideways = "Sideways";

const char* g_PropBlinkBleachProb = "FluoParam_BlinkBleachProb";
const char* g_PropOffLifetimeSec = "FluoParam_OffLifetimeSec";
const char* g_PropPhotonCV = "FluoParam_PhotonCV";
const char* g_PropIllumFwhmPct = "FluoParam_IllumFwhmPct";
const char* g_PropEmGain = "CamParam_EmGain";
const char* g_PropCicElectrons = "CamParam_CicElectrons";
const char* g_PropBgCellContrast = "Background_CellContrast";
const char* g_PropBgHazeWeight = "Background_HazeWeight";
const char* g_PropBgHazeWidthNm = "Background_HazeWidthNm";
const char* g_PropBgDecaySec = "Background_DecaySec";
const char* g_PropOutOfFocusRatio = "Background_OutOfFocusRatio";
const char* g_PropOutOfFocusDepthNm = "Background_OutOfFocusDepthNm";
const char* g_PropIllumProfile = "FluoParam_IllumProfile";
const char* g_IllumFlat = "Flat";
const char* g_IllumGaussian = "Gaussian";
const char* g_IllumFlatTop = "FlatTop";
const char* g_PropCameraType = "CamParam_CameraType";
const char* g_CameraTypeScmos = "sCMOS";
const char* g_CameraTypeEmccd = "EMCCD";
const char* g_PropBitDepth = "CamParam_BitDepth";

const char* g_PropPsfInterp = "PSFParam_PsfInterp";
const char* g_PsfInterpNearest = "Nearest";
const char* g_PsfInterpLinear = "Linear";
const char* g_PsfInterpCubic = "Cubic";
const char* g_PsfInterpFft = "Fft";

const char* g_PropUseGpu = "General_UseGpu";
const char* g_PropGpuStatus = "General_GpuStatus";
const char* g_UseGpuOn = "On";
const char* g_UseGpuOff = "Off";

const char* g_PropPsfMaskType = "PSFParam_PsfMaskType";
const char* g_PropPsfMaskModes = "PSFParam_PsfMaskModes";
const char* g_PropPsfMaskWaist = "PSFParam_PsfMaskWaist";
const char* g_PsfMaskNone = "None";
const char* g_PsfMaskDoubleHelix = "DoubleHelix";

const char* g_PsfModelGaussian = "Gaussian";
const char* g_PsfModelRichardsWolf = "RichardsWolf";
const char* g_PsfModelGibsonLanni = "GibsonLanni";
const char* g_PsfModelGibsonLanniZernike = "GibsonLanniZernike";

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
const char* g_PatternTiltedPlane = "TiltedPlane";
const char* g_PatternUniform3D = "Uniform3D";
const char* g_PatternShell = "Shell";
const char* g_PatternNup = "NUP";
const char* g_PatternCalibration9Spots = "Calibration9Spots";
const char* g_PatternFilamentsRing = "FilamentsRing";

const char* g_Fov128 = "128x128";
const char* g_Fov256 = "256x256";
const char* g_Fov512 = "512x512";

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
   // 3D/NPC site-list structures -- see Simulation/SMLMStructures.h.
   AddAllowedValue(g_PropPattern, g_PatternTiltedPlane);
   AddAllowedValue(g_PropPattern, g_PatternUniform3D);
   AddAllowedValue(g_PropPattern, g_PatternShell);
   AddAllowedValue(g_PropPattern, g_PatternNup);
   // Always-on calibration bead grid -- see Simulation/SMLMPatterns.h.
   AddAllowedValue(g_PropPattern, g_PatternCalibration9Spots);
   // webSMLM's default structure: 3 filaments with y-correlated z + a ring.
   AddAllowedValue(g_PropPattern, g_PatternFilamentsRing);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCustomPointsFile);
   CreateStringProperty(g_PropCustomPointsFile, "", false, pAct);

   // Ring/scale-step/spiral-arc gap (Circle/Spiral/Star/Heart) and per-cell
   // line spacing (ResolutionTarget), easiest to hardest, nm, comma-
   // separated. Any positive count of values is accepted; the smallest
   // value drives the finest ring/cell, the largest the coarsest.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnResolutionSpacingsNm);
   CreateStringProperty(g_PropResolutionSpacingsNm,
                         sim::FormatResolutionSpacingsNm(resolutionSpacingsNm_).c_str(), false, pAct);

   // Precomputed-stack properties. Stack length and looping are no longer
   // user-facing (see stackLength_/stackLoop_ in SMLMDemoCamera.h) -- what
   // remains is the trigger plus the two read-only status readbacks.
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

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnQuantumEfficiency);
   CreateFloatProperty(g_PropQuantumEfficiency, quantumEfficiency_.load(), false, pAct);
   SetPropertyLimits(g_PropQuantumEfficiency, 0.01, 1.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnDarkCurrentPerSec);
   CreateFloatProperty(g_PropDarkCurrentPerSec, darkCurrentPerSec_.load(), false, pAct);
   SetPropertyLimits(g_PropDarkCurrentPerSec, 0.0, 100.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCameraGain);
   CreateFloatProperty(g_PropGain, gainPhotonsPerAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropGain, 0.01, 100.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCameraOffset);
   CreateFloatProperty(g_PropOffset, offsetAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropOffset, 0.0, 10000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnOffsetStd);
   CreateFloatProperty(g_PropOffsetStd, offsetStdAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropOffsetStd, 0.0, 500.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnReadNoise);
   CreateFloatProperty(g_PropReadNoise, readNoiseElectrons_.load(), false, pAct);
   SetPropertyLimits(g_PropReadNoise, 0.0, 100.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPixelGainStdPct);
   CreateFloatProperty(g_PropPixelGainStdPct, pixelGainStdPct_.load(), false, pAct);
   SetPropertyLimits(g_PropPixelGainStdPct, 0.0, 50.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPixelReadNoiseStdPct);
   CreateFloatProperty(g_PropPixelReadNoiseStdPct, pixelReadNoiseStdPct_.load(), false, pAct);
   SetPropertyLimits(g_PropPixelReadNoiseStdPct, 0.0, 100.0);

   // ---- webSMLM parity round 2 (see the members' comments in
   // SMLMDemoCamera.h). Multi-blink photophysics + illumination profile:
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBlinkBleachProb);
   CreateFloatProperty(g_PropBlinkBleachProb, blinkBleachProb_.load(), false, pAct);
   SetPropertyLimits(g_PropBlinkBleachProb, 0.01, 1.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnOffLifetimeSec);
   CreateFloatProperty(g_PropOffLifetimeSec, offLifetimeSec_.load(), false, pAct);
   SetPropertyLimits(g_PropOffLifetimeSec, 0.001, 1000.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPhotonCV);
   CreateFloatProperty(g_PropPhotonCV, photonCV_.load(), false, pAct);
   SetPropertyLimits(g_PropPhotonCV, 0.0, 2.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnIllumProfile);
   CreateStringProperty(g_PropIllumProfile, g_IllumFlat, false, pAct);
   AddAllowedValue(g_PropIllumProfile, g_IllumFlat);
   AddAllowedValue(g_PropIllumProfile, g_IllumGaussian);
   AddAllowedValue(g_PropIllumProfile, g_IllumFlatTop);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnIllumFwhmPct);
   CreateFloatProperty(g_PropIllumFwhmPct, illumFwhmPct_.load(), false, pAct);
   SetPropertyLimits(g_PropIllumFwhmPct, 10.0, 300.0);

   // Sensor type (EMCCD gain register vs the original sCMOS chain):
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCameraType);
   CreateStringProperty(g_PropCameraType, g_CameraTypeScmos, false, pAct);
   AddAllowedValue(g_PropCameraType, g_CameraTypeScmos);
   AddAllowedValue(g_PropCameraType, g_CameraTypeEmccd);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnEmGain);
   CreateFloatProperty(g_PropEmGain, emGain_.load(), false, pAct);
   SetPropertyLimits(g_PropEmGain, 1.0, 2000.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnCicElectrons);
   CreateFloatProperty(g_PropCicElectrons, cicElectrons_.load(), false, pAct);
   SetPropertyLimits(g_PropCicElectrons, 0.0, 1.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBitDepth);
   CreateIntegerProperty(g_PropBitDepth, bitDepth_, false, pAct);
   SetPropertyLimits(g_PropBitDepth, 8, 16);

   // Structured background + out-of-focus emitters:
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBgCellContrast);
   CreateFloatProperty(g_PropBgCellContrast, bgCellContrast_.load(), false, pAct);
   SetPropertyLimits(g_PropBgCellContrast, 1.0, 20.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBgHazeWeight);
   CreateFloatProperty(g_PropBgHazeWeight, bgHazeWeight_.load(), false, pAct);
   SetPropertyLimits(g_PropBgHazeWeight, 0.0, 10.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBgHazeWidthNm);
   CreateFloatProperty(g_PropBgHazeWidthNm, bgHazeWidthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropBgHazeWidthNm, 100.0, 5000.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnBgDecaySec);
   CreateFloatProperty(g_PropBgDecaySec, bgDecaySec_.load(), false, pAct);
   SetPropertyLimits(g_PropBgDecaySec, 0.0, 100000.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnOutOfFocusRatio);
   CreateFloatProperty(g_PropOutOfFocusRatio, outOfFocusRatio_.load(), false, pAct);
   SetPropertyLimits(g_PropOutOfFocusRatio, 0.0, 10.0);
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnOutOfFocusDepthNm);
   CreateFloatProperty(g_PropOutOfFocusDepthNm, outOfFocusDepthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropOutOfFocusDepthNm, 300.0, 5000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnDriftNmPerSec);
   CreateFloatProperty(g_PropDriftNmPerSec, driftNmPerSecX_.load(), false, pAct);
   SetPropertyLimits(g_PropDriftNmPerSec, 0.0, 20000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnActualFrameIntervalMs);
   CreateFloatProperty(g_PropActualFrameIntervalMs, 0.0, true, pAct);

   // Vectorial PSF (embedded PSFGenerator JVM bridge). Default model is
   // GibsonLanniZernike -- see psfModel_'s own initializer in
   // SMLMDemoCamera.h; this string just needs to agree with it.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfModel);
   CreateStringProperty(g_PropPsfModel, g_PsfModelGibsonLanniZernike, false, pAct);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGaussian);
   AddAllowedValue(g_PropPsfModel, g_PsfModelRichardsWolf);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGibsonLanni);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGibsonLanniZernike);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfImmersionIndex);
   CreateFloatProperty(g_PropPsfImmersionIndex, psfImmersionIndex_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfImmersionIndex, 1.0, 2.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfOversampling);
   CreateIntegerProperty(g_PropPsfOversampling, psfOversampling_, false, pAct);
   SetPropertyLimits(g_PropPsfOversampling, 1, 16);

   // Expressed in nanometers (not camera pixels) so it stays physically
   // meaningful when PixelSizeNm changes -- rounded to a whole pixel count
   // internally, and still only a MINIMUM (see BuildPsfGeneratorRequest).
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfKernelHalfWidthNm);
   CreateFloatProperty(g_PropPsfKernelHalfWidthNm, psfKernelHalfWidthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfKernelHalfWidthNm, 100.0, 20000.0);

   // PSFGenerator itself (and this project's bridge class) are embedded in
   // this DLL -- nothing to point at except, optionally, a specific JRE/JDK
   // install to supply jvm.dll. Left empty (the default), the JVM
   // auto-detects one (JAVA_HOME, then common install locations -- see
   // sim::FindJavaHome in PsfGeneratorBridge.cpp).
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfGeneratorJavaHome);
   CreateStringProperty(g_PropPsfGeneratorJavaHome, psfGeneratorJavaHome_.c_str(), false, pAct);

   // Z-stack range/step (vectorial PSF plan step 2) -- the actual focus
   // offset used each frame comes from the SMLMDemoZStage device (step 3),
   // not from a property on this camera.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZRangeUm);
   CreateFloatProperty(g_PropPsfZRangeUm, psfZRangeUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZRangeUm, 0.1, 20.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZStepUm);
   CreateFloatProperty(g_PropPsfZStepUm, psfZStepUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZStepUm, 0.01, 1.0);

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

   // GibsonLanniZernike-only: 28-value comma-separated positional Zernike
   // coefficient list (OSA index 0-27, in waves; a 15-value list is also
   // accepted and zero-padded -- see Simulation/
   // SMLMZernike.h's ZernikeCoefficients doc comment for the full mode
   // list). Defaults to the MixedRealisticObjective preset's values (see
   // psfZernikePreset_ in SMLMDemoCamera.h), not all-zero.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZernikeCoefficients);
   CreateStringProperty(g_PropPsfZernikeCoefficients, psfZernikeCoefficients_.c_str(), false, pAct);

   // Convenience presets on top of PsfZernikeCoefficients -- selecting one
   // overwrites it with a named, literature-inspired aberration template
   // (see sim::ZernikePresetCoefficients in Simulation/SMLMZernike.cpp for
   // the values and their sourcing/caveats). Editing PsfZernikeCoefficients
   // directly afterwards is unaffected by (and does not update) this
   // property -- it only ever reports the last preset explicitly selected
   // through it.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfZernikePreset);
   CreateStringProperty(g_PropPsfZernikePreset, psfZernikePreset_.c_str(), false, pAct);
   for (const std::string& name : sim::ZernikePresetNames())
      AddAllowedValue(g_PropPsfZernikePreset, name.c_str());

   // 3D structures / labeling efficiency -- see Simulation/SMLMStructures.h.
   // LabelingEfficiencyPct/StructureSizeNm/NupX only matter for the four
   // site-list Pattern values above (TiltedPlane/Uniform3D/Shell/NUP);
   // StructureZRangeNm ALSO applies to the 9 continuous patterns (via the
   // ZSpreadPattern decorator -- see CreatePattern in SMLMPatterns.cpp).
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnLabelingEfficiencyPct);
   CreateFloatProperty(g_PropLabelingEfficiencyPct, labelingEfficiencyPct_.load(), false, pAct);
   SetPropertyLimits(g_PropLabelingEfficiencyPct, 0.0, 100.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnStructureZRangeNm);
   CreateFloatProperty(g_PropStructureZRangeNm, structureZRangeNm_.load(), false, pAct);
   SetPropertyLimits(g_PropStructureZRangeNm, 0.0, 5000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnStructureSizeNm);
   CreateFloatProperty(g_PropStructureSizeNm, structureSizeNm_.load(), false, pAct);
   SetPropertyLimits(g_PropStructureSizeNm, 10.0, 5000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupRadiusNm);
   CreateFloatProperty(g_PropNupRadiusNm, nupRadiusNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupRadiusNm, 20.0, 150.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupCornerSpreadNm);
   CreateFloatProperty(g_PropNupCornerSpreadNm, nupCornerSpreadNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupCornerSpreadNm, 0.0, 30.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupRingSeparationNm);
   CreateFloatProperty(g_PropNupRingSeparationNm, nupRingSeparationNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupRingSeparationNm, 0.0, 150.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupLinkerMinNm);
   CreateFloatProperty(g_PropNupLinkerMinNm, nupLinkerMinNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupLinkerMinNm, 0.0, 30.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupLinkerMaxNm);
   CreateFloatProperty(g_PropNupLinkerMaxNm, nupLinkerMaxNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupLinkerMaxNm, 0.0, 30.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupMembraneType);
   CreateStringProperty(g_PropNupMembraneType, g_NupMembraneTopDown, false, pAct);
   AddAllowedValue(g_PropNupMembraneType, g_NupMembraneTopDown);
   AddAllowedValue(g_PropNupMembraneType, g_NupMembraneSideways);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupCount);
   CreateIntegerProperty(g_PropNupCount, nupCount_, false, pAct);
   SetPropertyLimits(g_PropNupCount, 1, 500);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupMinSpacingNm);
   CreateFloatProperty(g_PropNupMinSpacingNm, nupMinSpacingNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupMinSpacingNm, 0.0, 2000.0);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnNupCurvatureNm);
   CreateFloatProperty(g_PropNupCurvatureNm, nupCurvatureNm_.load(), false, pAct);
   SetPropertyLimits(g_PropNupCurvatureNm, 0.0, 2000.0);

   // Sub-pixel PSF placement (vectorial PSF models only) -- see
   // Simulation/PsfGeneratorBridge.h's PsfInterpMode. Default is Cubic;
   // Nearest reproduces the original box-average splat exactly.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfInterp);
   CreateStringProperty(g_PropPsfInterp, g_PsfInterpCubic, false, pAct);
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpNearest);
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpLinear);
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpCubic);
   // Fourier-shift placement (webSMLM's 'fft'): exact, but one 2D FFT pair
   // per emitter -- much slower, and CPU-only. Kept for comparison.
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpFft);

   // GPU splat + noise (Direct3D 11) for vectorial PSF frames -- see
   // Simulation/GpuSimD3D11.h. Falls back to the multi-threaded CPU path
   // (same counter-based noise, so the same frames up to float32 rounding)
   // when unavailable; GpuStatus says which is in use and why.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnUseGpu);
   CreateStringProperty(g_PropUseGpu, g_UseGpuOn, false, pAct);
   AddAllowedValue(g_PropUseGpu, g_UseGpuOn);
   AddAllowedValue(g_PropUseGpu, g_UseGpuOff);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnGpuStatus);
   CreateStringProperty(g_PropGpuStatus, "", true, pAct);

   // GibsonLanniZernike-only pupil phase mask (engineered PSF) -- see
   // Simulation/PsfGeneratorBridge.h's PsfMaskType. DoubleHelix is webSMLM's
   // Gauss-Laguerre double-helix mask: two lobes rotating ~60 degrees over
   // +/-800 nm at the default 5 modes / waist 1.0 pupil radii.
   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfMaskType);
   CreateStringProperty(g_PropPsfMaskType, g_PsfMaskNone, false, pAct);
   AddAllowedValue(g_PropPsfMaskType, g_PsfMaskNone);
   AddAllowedValue(g_PropPsfMaskType, g_PsfMaskDoubleHelix);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfMaskModes);
   CreateIntegerProperty(g_PropPsfMaskModes, psfMaskModes_, false, pAct);
   SetPropertyLimits(g_PropPsfMaskModes, 2, 8);

   pAct = new CPropertyAction(this, &CSMLMDemoCamera::OnPsfMaskWaist);
   CreateFloatProperty(g_PropPsfMaskWaist, psfMaskWaist_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfMaskWaist, 0.2, 2.0);

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
// The EMCCD path clips to its own bit depth (CamParam_BitDepth); tell MM so
// its display range matches. sCMOS stays 16-bit.
unsigned CSMLMDemoCamera::GetBitDepth() const { return cameraEmccd_ ? static_cast<unsigned>(bitDepth_) : 16; }
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
