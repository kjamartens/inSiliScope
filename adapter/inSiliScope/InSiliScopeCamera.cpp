///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeCamera.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   CInSiliScopeCamera: MM::Camera API, property handlers, and the
//                sequence-acquisition thread. The actual frame generation
//                (precomputed-stack management, live producer thread, and
//                calls into the simulation engine) lives in
//                SMLMImageGeneration.cpp.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"
#include "Simulation/CacheDir.h"
#include "Simulation/SharedStageState.h"

#include "CameraImageMetadata.h"
#include "ModuleInterface.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sstream>

const char* g_CameraDeviceName = "Camera";

const char* g_PropAcqMode = "General_AcqMode";
const char* g_PropFovSize = "General_FovSize";
const char* g_PropGenerateStack = "General_GenerateStack";
const char* g_PropStackStatus = "General_StackGenerationStatus";
const char* g_PropStackLength = "General_StackLength";
const char* g_PropEndOfStack = "General_EndOfStackReached";
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
const char* g_PropDriftXyNmPerSqrtSec = "SimType_DriftXyNmPerSqrtSec";
const char* g_PropDriftZNmPerSqrtSec = "SimType_DriftZNmPerSqrtSec";
const char* g_PropDirectedDrift[DD_COUNT] = {
   "SimType_DriftXySpeedNmPerSec",
   "SimType_DriftZSpeedNmPerSec",
   "SimType_DriftXyAngleDeg",
   "SimType_DriftXyAngleWanderDeg",
   "SimType_DriftSpeedWanderPct",
   "SimType_DriftWanderTimeSec",
};
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

const char* g_PropIllumFwhmPct = "Optics_IlluminationFwhmPct";
const char* g_PropEmGain = "CamParam_EmGain";
const char* g_PropCicElectrons = "CamParam_CicElectrons";
const char* g_PropBgDecaySec = "Background_DecaySec";
const char* g_PropIllumProfile = "Optics_IlluminationProfile";
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
const char* g_PropDiskCache = "General_DiskCache";
const char* g_DiskCacheOff = "Off";
const char* g_DiskCacheCells = "Cells";
const char* g_DiskCacheCellsAndPsf = "CellsAndPsf";
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

const char* g_PropCellFieldNumber[CF_COUNT] = {
   "SimType_CellFieldChunkSizeUm",
   "SimType_CellFieldOccupancy",
   "SimType_CellFieldCellDiameterMinUm",
   "SimType_CellFieldCellDiameterMaxUm",
   "SimType_CellFieldMicrotubuleDensityPerUm2",
   "SimType_CellFieldFocusHeightUm",
   "SimType_CellFieldZRangeUm",
   "SimType_CellFieldNucBaseMinUm",
   "SimType_CellFieldNucBaseMaxUm",
   "SimType_CellFieldNucIrregMin",
   "SimType_CellFieldNucIrregMax",
   "SimType_CellFieldNucBendMin",
   "SimType_CellFieldNucBendMax",
   "SimType_CellFieldNucSmooth",
   "SimType_CellFieldNucThickIrreg",
   "SimType_CellFieldNucAsym",
   "SimType_CellFieldNucWidestMin",
   "SimType_CellFieldNucWidestMax",
   "SimType_CellFieldMicrotubuleStartDecayPct",
   "SimType_CellFieldMicrotubuleEndDecayPct",
   "SimType_CellFieldMicrotubuleDirKappa",
};
const char* g_CellFieldCoreParam[CF_COUNT] = {
   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
   "nucBaseMin",
   "nucBaseMax",
   "nucIrregMin",
   "nucIrregMax",
   "nucBendMin",
   "nucBendMax",
   "nucSmooth",
   "nucThickIrreg",
   "nucAsym",
   "nucWidestMin",
   "nucWidestMax",
   "mtStartDecayPct",
   "mtEndDecayPct",
   "mtDirKappa",
};
const char* g_PropCellFieldPacking = "SimType_CellFieldPacking";

const char* g_PropImagingModality = "General_ImagingModality";
const char* g_ModalityFluorescence = "Fluorescence";
const char* g_PropWideFieldNumber[WF_COUNT] = {
   "General_WideFieldUpscaling",
   "General_WideFieldZPlaneNm",
};
const char* g_ModalityBrightField = "BrightField";
const char* g_PropBrightFieldNumber[BF_COUNT] = {
   "General_BrightFieldQuality",
   "General_BrightFieldSources",
   "General_BrightFieldUpscaling",
   "General_BrightFieldGeometrySamples",
   "General_BrightFieldSliceUm",
   "General_BrightFieldCondenserNa",
   "General_BrightFieldWavelengthNm",
   "General_BrightFieldPhotonsPerPxPerSec",
   "General_BrightFieldAberrations",
   "SimType_CellFieldIndexMedium",
   "SimType_CellFieldIndexCytoplasm",
   "SimType_CellFieldIndexNucleus",
   "SimType_CellFieldIndexMicrotubule",
   "SimType_CellFieldAbsorptionPerUm",
};

const char* g_Fov128 = "128x128";
const char* g_Fov256 = "256x256";
const char* g_Fov512 = "512x512";

///////////////////////////////////////////////////////////////////////////////
// CInSiliScopeCamera implementation
///////////////////////////////////////////////////////////////////////////////

CInSiliScopeCamera::CInSiliScopeCamera()
{
   InitializeDefaultErrorMessages();
   thd_ = new SMLMSequenceThread(this);

   // CellField defaults: the prototype's (spec/PORT.md 4.2; the labels and
   // their dyes are the scope options of ScopeProperties.cpp); a 7 um z slab around the focal plane (0 = every dye, no z limit), and no
   // focus offset: ZStage = 0 puts the coverslip in focus (the ZStage starts
   // at 0.5 um). Then the nucleus shape and microtubule start/end: the core's defaults (core/src/params.h).
   const double cellFieldDefaults[CF_COUNT] = { 26.0, 0.33, 25.0, 35.0, 0.9, 0.0, 7.0,
      0.4, 0.9, 0.03, 0.2, 0.0, 0.3, 2.5, 0.1, 0.5, 0.2, 0.4, 1.6, 20.0, 1.5 };
   for (int i = 0; i < CF_COUNT; ++i)
      cellField_[i] = cellFieldDefaults[i];
   // Mean-field grid (the WideField-mode populations): 1 cell/pixel, 25 nm
   // dye planes (the cli/viewer's wf-upscale / wf-plane-nm).
   const double wideFieldDefaults[WF_COUNT] = { 1.0, 25.0 };
   for (int i = 0; i < WF_COUNT; ++i)
      wideFieldNum_[i] = wideFieldDefaults[i];
   // Directed drift: off (speeds 0), random direction per seed, no wander,
   // 60 s correlation time.
   const double directedDriftDefaults[DD_COUNT] = { 0.0, 0.0, -1.0, 0.0, 0.0, 60.0 };
   for (int i = 0; i < DD_COUNT; ++i)
      directedDrift_[i] = directedDriftDefaults[i];
   // BrightField: quality 3 (its sources/upscaling/samples/slice: 0 / -1 =
   // from the quality), condenser NA 0.55, 550 nm, 40000 photons/pixel/s
   // (2000 per 50 ms frame), the PSF's aberrations, refractive indices of
   // medium / cytoplasm / nucleus / microtubule (spec/BRIGHTFIELD.md), no
   // absorption (unstained).
   const double brightFieldDefaults[BF_COUNT] = { 3, 0, 0, 0, -1, 0.4, 550, 80000, 1, 1.337, 1.35, 1.35, 1.48, 0 };
   for (int i = 0; i < BF_COUNT; ++i)
      brightFieldNum_[i] = brightFieldDefaults[i];

   // Pre-init property: must exist before Initialize() finishes. FovSize is
   // deliberately NOT pre-init -- unlike RandomSeed, it's a regular,
   // live-changeable property (see OnFovSize/ApplyFrameSizeChange), same as
   // Binning.
   CreateIntegerProperty(g_PropRandomSeed, randomSeed_, false,
                          new CPropertyAction(this, &CInSiliScopeCamera::OnRandomSeed), true);
}

CInSiliScopeCamera::~CInSiliScopeCamera()
{
   StopSequenceAcquisition();
   StopLiveProducer();
   if (stackGenThread_.joinable())
      stackGenThread_.join();
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

   int nRet = CreateStringProperty(MM::g_Keyword_Name, g_CameraDeviceName, true);
   if (nRet != DEVICE_OK)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_Description, "Synthetic SMLM demo camera", true);
   if (nRet != DEVICE_OK)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_CameraName, "inSiliScope", true);
   if (nRet != DEVICE_OK)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_CameraID, "V1.0", true);
   if (nRet != DEVICE_OK)
      return nRet;

   // FovSize: a regular, live-changeable property (not pre-init) -- can be
   // changed after the device has been added/initialized, same as Binning.
   CPropertyAction* pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnFovSize);
   nRet = CreateStringProperty(g_PropFovSize, g_Fov256, false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   AddAllowedValue(g_PropFovSize, g_Fov128);
   AddAllowedValue(g_PropFovSize, g_Fov256);
   AddAllowedValue(g_PropFovSize, g_Fov512);

   // Binning
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnBinning);
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
   // converts every rate-based simulation parameter (BackgroundPhotonsPerSec, the dark current) into the
   // frame-equivalent quantity for whatever this is currently set to.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnExposureProperty);
   nRet = CreateFloatProperty(MM::g_Keyword_Exposure, 50.0, false, pAct);
   if (nRet != DEVICE_OK)
      return nRet;
   SetPropertyLimits(MM::g_Keyword_Exposure, 1.0, 10000.0);

   // Acquisition mode. Default is Live -- continuous on-the-fly simulation,
   // no precomputed-stack generation delay before the first frame appears.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnAcqMode);
   CreateStringProperty(g_PropAcqMode, g_AcqModeLive, false, pAct);
   AddAllowedValue(g_PropAcqMode, g_AcqModePrecomputed);
   AddAllowedValue(g_PropAcqMode, g_AcqModeLive);

   // Precomputed-stack properties: the trigger, the stack length (frames,
   // default 1000; re-exposed 2026-10-06 so tests and short acquisitions do
   // not pay for 1000 frames), and the two read-only status readbacks. The
   // stack always loops (stackLoop_ in InSiliScopeCamera.h).
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnGenerateStack);
   CreateIntegerProperty(g_PropGenerateStack, 0, false, pAct);
   SetPropertyLimits(g_PropGenerateStack, 0, 1);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnStackLength);
   CreateIntegerProperty(g_PropStackLength, stackLength_, false, pAct);
   SetPropertyLimits(g_PropStackLength, 1, 100000);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnStackStatus);
   CreateStringProperty(g_PropStackStatus, "Idle", true, pAct);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnEndOfStackReached);
   CreateStringProperty(g_PropEndOfStack, "No", true, pAct);

   // Simulation parameters. BackgroundPhotonsPerSec and the dark current are
   // rates (per second) that scale automatically with the standard Exposure
   // property -- see SnapshotParams() in SMLMImageGeneration.cpp.

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfNa);
   CreateFloatProperty(g_PropPsfNa, psfNa_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfNa, 0.5, 1.49);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPixelSizeNm);
   CreateFloatProperty(g_PropPixelSize, pixelSizeNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPixelSize, 10.0, 1000.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnBackgroundPerSec);
   CreateFloatProperty(g_PropBackgroundPerSec, backgroundPhotonsPerSec_.load(), false, pAct);
   SetPropertyLimits(g_PropBackgroundPerSec, 0.0, 200000.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnQuantumEfficiency);
   CreateFloatProperty(g_PropQuantumEfficiency, quantumEfficiency_.load(), false, pAct);
   SetPropertyLimits(g_PropQuantumEfficiency, 0.01, 1.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnDarkCurrentPerSec);
   CreateFloatProperty(g_PropDarkCurrentPerSec, darkCurrentPerSec_.load(), false, pAct);
   SetPropertyLimits(g_PropDarkCurrentPerSec, 0.0, 100.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnCameraGain);
   CreateFloatProperty(g_PropGain, gainPhotonsPerAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropGain, 0.0001, 100.0);   // per photoelectron: an EMCCD preset uses 0.0066

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnCameraOffset);
   CreateFloatProperty(g_PropOffset, offsetAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropOffset, 0.0, 10000.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnOffsetStd);
   CreateFloatProperty(g_PropOffsetStd, offsetStdAdu_.load(), false, pAct);
   SetPropertyLimits(g_PropOffsetStd, 0.0, 500.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnReadNoise);
   CreateFloatProperty(g_PropReadNoise, readNoiseElectrons_.load(), false, pAct);
   SetPropertyLimits(g_PropReadNoise, 0.0, 100.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPixelGainStdPct);
   CreateFloatProperty(g_PropPixelGainStdPct, pixelGainStdPct_.load(), false, pAct);
   SetPropertyLimits(g_PropPixelGainStdPct, 0.0, 50.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPixelReadNoiseStdPct);
   CreateFloatProperty(g_PropPixelReadNoiseStdPct, pixelReadNoiseStdPct_.load(), false, pAct);
   SetPropertyLimits(g_PropPixelReadNoiseStdPct, 0.0, 100.0);

   // ---- webSMLM parity round 2 (see the members' comments in
   // InSiliScopeCamera.h). Illumination profile:
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnIllumProfile);
   CreateStringProperty(g_PropIllumProfile, g_IllumFlat, false, pAct);
   AddAllowedValue(g_PropIllumProfile, g_IllumFlat);
   AddAllowedValue(g_PropIllumProfile, g_IllumGaussian);
   AddAllowedValue(g_PropIllumProfile, g_IllumFlatTop);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnIllumFwhmPct);
   CreateFloatProperty(g_PropIllumFwhmPct, illumFwhmPct_.load(), false, pAct);
   SetPropertyLimits(g_PropIllumFwhmPct, 10.0, 300.0);

   // Sensor type (EMCCD gain register vs the original sCMOS chain):
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnCameraType);
   CreateStringProperty(g_PropCameraType, g_CameraTypeScmos, false, pAct);
   AddAllowedValue(g_PropCameraType, g_CameraTypeScmos);
   AddAllowedValue(g_PropCameraType, g_CameraTypeEmccd);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnEmGain);
   CreateFloatProperty(g_PropEmGain, EmGain(), true, pAct);   // read-only: derived from the gain
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnCicElectrons);
   CreateFloatProperty(g_PropCicElectrons, cicElectrons_.load(), false, pAct);
   SetPropertyLimits(g_PropCicElectrons, 0.0, 1.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnBitDepth);
   CreateIntegerProperty(g_PropBitDepth, bitDepth_, false, pAct);
   SetPropertyLimits(g_PropBitDepth, 8, 16);

   // Background fade:
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnBgDecaySec);
   CreateFloatProperty(g_PropBgDecaySec, bgDecaySec_.load(), false, pAct);
   SetPropertyLimits(g_PropBgDecaySec, 0.0, 100000.0);

   // Random-walk sample drift (Simulation/Drift.h): RMS displacement after
   // 1 s, per axis in x and y, and in z; 0 = none.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnDriftXyNmPerSqrtSec);
   CreateFloatProperty(g_PropDriftXyNmPerSqrtSec, driftXyNmPerSqrtSec_.load(), false, pAct);
   SetPropertyLimits(g_PropDriftXyNmPerSqrtSec, 0.0, 1000.0);
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnDriftZNmPerSqrtSec);
   CreateFloatProperty(g_PropDriftZNmPerSqrtSec, driftZNmPerSqrtSec_.load(), false, pAct);
   SetPropertyLimits(g_PropDriftZNmPerSqrtSec, 0.0, 1000.0);
   // Directed drift on top: mean xy speed (direction random per seed unless
   // set) and signed z speed; their direction/strength wander slowly
   // (advanced: angle, wanders, correlation time).
   {
      const double lo[DD_COUNT] = { 0.0, -10000.0, -1.0, 0.0, 0.0, 0.1 };
      const double hi[DD_COUNT] = { 10000.0, 10000.0, 360.0, 180.0, 100.0, 100000.0 };
      for (long i = 0; i < DD_COUNT; ++i)
      {
         CreateFloatProperty(g_PropDirectedDrift[i], directedDrift_[i].load(), false,
                             new CPropertyActionEx(this, &CInSiliScopeCamera::OnDirectedDrift, i));
         SetPropertyLimits(g_PropDirectedDrift[i], lo[i], hi[i]);
      }
   }

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnActualFrameIntervalMs);
   CreateFloatProperty(g_PropActualFrameIntervalMs, 0.0, true, pAct);

   // Diffraction PSF: GibsonLanniZernike (the default) in C++ (Simulation/
   // ZernikePsf.*), RichardsWolf/GibsonLanni in the embedded PSFGenerator JVM
   // bridge. See psfModel_'s own initializer in
   // InSiliScopeCamera.h; this string just needs to agree with it.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfModel);
   CreateStringProperty(g_PropPsfModel, g_PsfModelGibsonLanniZernike, false, pAct);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGaussian);
   AddAllowedValue(g_PropPsfModel, g_PsfModelRichardsWolf);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGibsonLanni);
   AddAllowedValue(g_PropPsfModel, g_PsfModelGibsonLanniZernike);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfImmersionIndex);
   CreateFloatProperty(g_PropPsfImmersionIndex, psfImmersionIndex_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfImmersionIndex, 1.0, 2.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfOversampling);
   CreateIntegerProperty(g_PropPsfOversampling, psfOversampling_, false, pAct);
   SetPropertyLimits(g_PropPsfOversampling, 1, 16);

   // Expressed in nanometers (not camera pixels) so it stays physically
   // meaningful when PixelSizeNm changes -- rounded to a whole pixel count
   // internally, and still only a MINIMUM (see BuildPsfGeneratorRequest).
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfKernelHalfWidthNm);
   CreateFloatProperty(g_PropPsfKernelHalfWidthNm, psfKernelHalfWidthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfKernelHalfWidthNm, 100.0, 20000.0);

   // PSFGenerator itself (and this project's bridge class) are embedded in
   // this DLL -- nothing to point at except, optionally, a specific JRE/JDK
   // install to supply jvm.dll. Left empty (the default), the JVM
   // auto-detects one (JAVA_HOME, then common install locations -- see
   // sim::FindJavaHome in PsfGeneratorBridge.cpp). Only RichardsWolf and
   // GibsonLanni use the JVM.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfGeneratorJavaHome);
   CreateStringProperty(g_PropPsfGeneratorJavaHome, psfGeneratorJavaHome_.c_str(), false, pAct);

   // Z-stack range/step (PSF plan (docs/dev/vectorial-psf-plan.md) step 2) -- the actual focus
   // offset used each frame comes from the InSiliScopeZStage device (step 3),
   // not from a property on this camera.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfZRangeUm);
   CreateFloatProperty(g_PropPsfZRangeUm, psfZRangeUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZRangeUm, 0.1, 20.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfZStepUm);
   CreateFloatProperty(g_PropPsfZStepUm, psfZStepUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfZStepUm, 0.01, 1.0);

   // GibsonLanni-only parameters (ignored by RichardsWolf); see the comments
   // on psfSampleIndex_/psfWorkingDistanceUm_/psfSampleDepthNm_ in
   // InSiliScopeCamera.h for why these particular defaults reproduce the
   // no-mismatch/in-focus behavior this bridge used to hardcode.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfSampleIndex);
   CreateFloatProperty(g_PropPsfSampleIndex, psfSampleIndex_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfSampleIndex, 1.0, 2.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfWorkingDistanceUm);
   CreateFloatProperty(g_PropPsfWorkingDistanceUm, psfWorkingDistanceUm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfWorkingDistanceUm, 0.0, 9999.0);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfSampleDepthNm);
   CreateFloatProperty(g_PropPsfSampleDepthNm, psfSampleDepthNm_.load(), false, pAct);
   SetPropertyLimits(g_PropPsfSampleDepthNm, -100000.0, 100000.0);

   // GibsonLanniZernike-only: 28-value comma-separated positional Zernike
   // coefficient list (OSA index 0-27, in waves; a 15-value list is also
   // accepted and zero-padded -- see Simulation/
   // SMLMZernike.h's ZernikeCoefficients doc comment for the full mode
   // list). Defaults to the MixedRealisticObjective preset's values (see
   // psfZernikePreset_ in InSiliScopeCamera.h), not all-zero.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfZernikeCoefficients);
   CreateStringProperty(g_PropPsfZernikeCoefficients, psfZernikeCoefficients_.c_str(), false, pAct);

   // Convenience presets on top of PsfZernikeCoefficients -- selecting one
   // overwrites it with a named, literature-inspired aberration template
   // (see sim::ZernikePresetCoefficients in Simulation/SMLMZernike.cpp for
   // the values and their sourcing/caveats). Editing PsfZernikeCoefficients
   // directly afterwards is unaffected by (and does not update) this
   // property -- it only ever reports the last preset explicitly selected
   // through it.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfZernikePreset);
   CreateStringProperty(g_PropPsfZernikePreset, psfZernikePreset_.c_str(), false, pAct);
   for (const std::string& name : sim::ZernikePresetNames())
      AddAllowedValue(g_PropPsfZernikePreset, name.c_str());

   // CellField pattern (spec/PORT.md 9). The rest of the world's parameters
   // stay at the prototype defaults until someone needs them. A min above its
   // max acts as the min (the core's NormalizeParams).
   {
      const double lo[CF_COUNT] = { 4.0, 0.05, 5.0, 5.0, 0.0, -10.0, 0.0,
         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -0.9, 0.0, 0.0, 0.05, 0.5, 0.0 };
      const double hi[CF_COUNT] = { 200.0, 1.0, 100.0, 100.0, 2.0, 10.0, 50.0,
         2.0, 2.0, 0.3, 0.3, 1.0, 1.0, 4.0, 0.4, 0.9, 1.0, 1.0, 10.0, 50.0, 10.0 };
      for (long i = 0; i < CF_COUNT; ++i)
      {
         CreateFloatProperty(g_PropCellFieldNumber[i], cellField_[i].load(), false,
                             new CPropertyActionEx(this, &CInSiliScopeCamera::OnCellFieldNumber, i));
         SetPropertyLimits(g_PropCellFieldNumber[i], lo[i], hi[i]);
      }
      CreateStringProperty(g_PropCellFieldPacking, "On", false,
                           new CPropertyAction(this, &CInSiliScopeCamera::OnCellFieldPacking));
      AddAllowedValue(g_PropCellFieldPacking, "On");
      AddAllowedValue(g_PropCellFieldPacking, "Off");
   }

   // Issue 16: the labels, dyes, light path and camera curve (the engine's
   // scope options, ScopeProperties.cpp).
   CreateScopeProperties();

   // Imaging modality: Fluorescence renders every structure's label in its
   // mode (dSTORM, PALM, DNA-PAINT blinks or WideField mean field) through
   // the light path; BrightField transmitted light.
   CreateStringProperty(g_PropImagingModality, g_ModalityFluorescence, false,
                        new CPropertyAction(this, &CInSiliScopeCamera::OnImagingModality));
   AddAllowedValue(g_PropImagingModality, g_ModalityFluorescence);
   AddAllowedValue(g_PropImagingModality, g_ModalityBrightField);
   {
      const double lo[BF_COUNT] = { 1, 0, 0, 0, -1, 0, 300, 0, 0, 1.0, 1.0, 1.0, 1.0, 0 };
      const double hi[BF_COUNT] = { 4, 1024, 8, 16, 5, 1.5, 1000, 1e9, 1, 2.0, 2.0, 2.0, 2.0, 100 };
      for (long i = 0; i < BF_COUNT; ++i)
      {
         auto* act = new CPropertyActionEx(this, &CInSiliScopeCamera::OnBrightFieldNumber, i);
         const bool integer = i == BF_QUALITY || i == BF_SOURCES || i == BF_UPSCALING || i == BF_GEOMETRY_SAMPLES ||
                              i == BF_ABERRATIONS;
         if (integer)
            CreateIntegerProperty(g_PropBrightFieldNumber[i], static_cast<long>(brightFieldNum_[i].load()), false, act);
         else
            CreateFloatProperty(g_PropBrightFieldNumber[i], brightFieldNum_[i].load(), false, act);
         SetPropertyLimits(g_PropBrightFieldNumber[i], lo[i], hi[i]);
      }
   }
   {
      const double lo[WF_COUNT] = { 1.0, 5.0 };
      const double hi[WF_COUNT] = { 4.0, 500.0 };
      for (long i = 0; i < WF_COUNT; ++i)
      {
         auto* act = new CPropertyActionEx(this, &CInSiliScopeCamera::OnWideFieldNumber, i);
         if (i == WF_UPSCALING)
            CreateIntegerProperty(g_PropWideFieldNumber[i], static_cast<long>(wideFieldNum_[i].load()), false, act);
         else
            CreateFloatProperty(g_PropWideFieldNumber[i], wideFieldNum_[i].load(), false, act);
         SetPropertyLimits(g_PropWideFieldNumber[i], lo[i], hi[i]);
      }
   }

   // Sub-pixel PSF placement (diffraction PSF models only) -- see
   // Simulation/PsfGeneratorBridge.h's PsfInterpMode. Default is Cubic;
   // Nearest reproduces the original box-average splat exactly.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfInterp);
   CreateStringProperty(g_PropPsfInterp, g_PsfInterpCubic, false, pAct);
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpNearest);
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpLinear);
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpCubic);
   // Fourier-shift placement (webSMLM's 'fft'): exact, but one 2D FFT pair
   // per emitter -- much slower, and CPU-only. Kept for comparison.
   AddAllowedValue(g_PropPsfInterp, g_PsfInterpFft);

   // GPU splat + noise (Direct3D 11) for diffraction PSF frames -- see
   // Simulation/GpuSimD3D11.h. Falls back to the multi-threaded CPU path
   // (same counter-based noise, so the same frames up to float32 rounding)
   // when unavailable; GpuStatus says which is in use and why.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnUseGpu);
   CreateStringProperty(g_PropUseGpu, g_UseGpuOn, false, pAct);
   AddAllowedValue(g_PropUseGpu, g_UseGpuOn);
   AddAllowedValue(g_PropUseGpu, g_UseGpuOff);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnGpuStatus);
   CreateStringProperty(g_PropGpuStatus, "", true, pAct);

   // Persistent caches in the per-user cache directory (Simulation/CacheDir.h:
   // $ISC_CACHE_DIR, else %LOCALAPPDATA%\inSiliScope\cache). Cells = the
   // packed cell positions of the CellField world (five numbers per cell, a
   // few MB at most: a restart with the same seed and cell parameters packs
   // nothing); CellsAndPsf adds the diffraction PSF kernel (one file of up
   // to ~200 MB, so the kernel is read instead of computed at start).
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnDiskCache);
   CreateStringProperty(g_PropDiskCache, g_DiskCacheCells, false, pAct);
   AddAllowedValue(g_PropDiskCache, g_DiskCacheOff);
   AddAllowedValue(g_PropDiskCache, g_DiskCacheCells);
   AddAllowedValue(g_PropDiskCache, g_DiskCacheCellsAndPsf);

   // GibsonLanniZernike-only pupil phase mask (engineered PSF) -- see
   // Simulation/PsfGeneratorBridge.h's PsfMaskType. DoubleHelix is webSMLM's
   // Gauss-Laguerre double-helix mask: two lobes rotating ~60 degrees over
   // +/-800 nm at the default 5 modes / waist 1.0 pupil radii.
   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfMaskType);
   CreateStringProperty(g_PropPsfMaskType, g_PsfMaskNone, false, pAct);
   AddAllowedValue(g_PropPsfMaskType, g_PsfMaskNone);
   AddAllowedValue(g_PropPsfMaskType, g_PsfMaskDoubleHelix);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfMaskModes);
   CreateIntegerProperty(g_PropPsfMaskModes, psfMaskModes_, false, pAct);
   SetPropertyLimits(g_PropPsfMaskModes, 2, 8);

   pAct = new CPropertyAction(this, &CInSiliScopeCamera::OnPsfMaskWaist);
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
   // The diffraction PSF kernel is computed (or read from the disk cache,
   // General_DiskCache = CellsAndPsf) in the background from here on, so
   // the first snap, stack or live frame finds it in the memo instead of
   // waiting for it; a live loop asking for the same kernel meanwhile waits
   // for this thread rather than computing it again.
   sim::SetPsfKernelDiskCacheDir(diskCacheMode_.load() >= 2 ? sim::DefaultCacheDir() : std::string());
   // The JVM models (RichardsWolf, GibsonLanni) for the engine's dye states:
   // this camera's request at the state's emission wavelength.
   sim::SetScopePsfRequestHook([this](const sim::ScopeSpec&, double wavelengthNm, sim::PsfGeneratorRequest& req) {
      req = BuildPsfGeneratorRequest();
      req.wavelengthNm = wavelengthNm;
      req.kernelHalfWidthPx =
         sim::PsfKernelHalfWidthPx(psfKernelHalfWidthNm_.load(), req.pixelSizeNm, req.wavelengthNm, req.na);
      return true;
   });
   StartPsfPreload();
   if (acqMode_ == SMLM_MODE_LIVE)
      StartLiveProducer();

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
   sim::SetScopePsfRequestHook(nullptr);
   initialized_ = false;
   return DEVICE_OK;
}

int CInSiliScopeCamera::SnapImage()
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
unsigned CInSiliScopeCamera::GetBitDepth() const { return cameraEmccd_ ? static_cast<unsigned>(bitDepth_) : 16; }
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
   sim::GetSharedStageState().EndSequenceAcquisition();
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

   // A fresh Live/MDA acquisition restarts the drift from zero rather than
   // continuing wherever the previous acquisition left off.
   // An armed z sequence (hardware z stack) restarts at its first position;
   // the camera steps it one position per frame.
   const sim::SharedStageState::ZSequence zseq = sim::GetSharedStageState().GetZSequence();
   liveSeqEpoch_ = sim::GetSharedStageState().BeginSequenceAcquisition();
   // Live: the first frame is one started from here (the pose, focus and
   // settings of this moment).
   liveSeqStartTicks_ = sim::SharedStageState::Clock::now().time_since_epoch().count();
   liveSeqSkipStale_ = zseq.armed;
   liveSeqCapture_ = true;
   if (acqMode_ == SMLM_MODE_LIVE)
   {
      // The producer restarts the drift at the start of its next frame (a
      // frame already in flight was rendered with the old origin and is
      // skipped by the sequence).
      ++liveDriftRestart_;
   }
   else
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
      sim::GetSharedStageState().EndSequenceAcquisition();
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
