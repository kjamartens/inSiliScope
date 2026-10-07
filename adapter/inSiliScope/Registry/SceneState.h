///////////////////////////////////////////////////////////////////////////////
// FILE:          SceneState.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Every setting of the simulated microscope, in one place,
//                owned by the hub (InSiliScopeHub.h). The devices' properties
//                read and write it (Registry/PropertyTable.cpp binds each
//                property to one value here); the camera renders from it
//                (Registry/SceneSettings.h turns it into the engine's
//                structures). Values the render threads read are atomics; the
//                engine options and strings sit behind one mutex.
//
//                How settings are divided over devices and tiers:
//                spec/MM_DEVICES.md.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "../Simulation/PsfGeneratorBridge.h"
#include "../Simulation/SMLMBackground.h"
#include "../Simulation/SMLMZernike.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace isc {

// CellField numbers (the cell field's geometry; spec/PORT.md 9), one array.
enum CellFieldNumber
{
   CF_CHUNK_SIZE_UM = 0,
   CF_OCCUPANCY,
   CF_CELL_DIAM_MIN_UM,
   CF_CELL_DIAM_MAX_UM,
   CF_MT_DENSITY,
   CF_FOCUS_HEIGHT_UM,
   CF_Z_RANGE_UM,
   // Nucleus shape and microtubule start/end: core parameters of the same names (g_CellFieldCoreParam).
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
// The core parameter each CellField number sets as is (nullptr: handled by hand in BuildCellFieldSettings).
extern const char* g_CellFieldCoreParam[CF_COUNT];

// The mean-field grid of the WideField-mode populations.
enum WideFieldNumber
{
   WF_UPSCALING = 0,
   WF_Z_PLANE_NM,
   WF_COUNT
};

// BrightField: the lamp, the render numerics and the specimen's optics.
// Quality 1-4 sets sources/upscaling/geometry samples/slice step unless those
// are set (> 0; slice >= 0).
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

// Directed sample drift (sim::DriftSettings).
enum DirectedDriftNumber
{
   DD_XY_SPEED = 0,   // mean xy speed, nm/s
   DD_Z_SPEED,        // z speed, nm/s
   DD_XY_ANGLE,       // direction, deg (-1 = random per seed)
   DD_ANGLE_WANDER,   // swing of the xy direction, deg
   DD_SPEED_WANDER,   // RMS of the strengths, % of the mean
   DD_WANDER_TIME,    // correlation time of the wanders, s
   DD_Z_DIRECTION,    // 1 away from the coverslip, -1 towards it, 0 random per seed
   DD_Z_ANGLE_WANDER, // swing of the z drift, deg
   DD_COUNT
};

struct SceneState
{
   SceneState();

   // ---- session (hub) ----
   std::atomic<long> seed{42};

   // ---- XY stage: travel limit (the field is effectively unbounded) ----
   std::atomic<double> xyLimitUm{1e6};

   // ---- camera: geometry and timing (the camera writes these) ----
   std::atomic<double> exposureMs{50.0};
   std::atomic<long> fovPx{256};
   std::atomic<long> binning{1};
   long WidthPx() const { return fovPx.load() / binning.load(); }

   // ---- pixel size: sensor pixel / (objective x emission magnification) ----
   std::atomic<double> sensorPixelUm{6.5};
   std::atomic<double> objectiveMag{100.0};
   // 0.667x: the Kinetix's 6.5 um pixel at 100x gives 97.45 nm (near the ~100 nm of SMLM setups).
   std::atomic<double> emissionMag{0.667};
   double PixelSizeNm() const { return sensorPixelUm.load() * 1000.0 / (objectiveMag.load() * emissionMag.load()); }

   // ---- camera: noise chain (Kinetix22 sCMOS, CMS mode, by default) ----
   std::atomic<double> quantumEfficiency{0.85};
   std::atomic<double> darkCurrentPerSec{1.03};
   std::atomic<double> gainElectronsPerAdu{0.25};
   std::atomic<double> offsetAdu{100.0};
   std::atomic<double> offsetStdAdu{0.5};
   std::atomic<double> readNoiseElectrons{1.2};
   std::atomic<double> pixelGainStdPct{0.5};        // sCMOS per-pixel spreads
   std::atomic<double> pixelReadNoiseStdPct{20.0};
   std::atomic<int> emccd{0};
   std::atomic<double> cicElectrons{0.002};
   std::atomic<int> bitDepth{16};
   std::atomic<int> cameraPreset{0};                 // index into sim::CameraIds()
   std::atomic<int> driftPreset{0};                  // index into DriftPresetNames() (the last: Custom)
   double EmGain() const;                            // the preset's pre-amplifier sensitivity / the gain

   // ---- objective: the detection pupil and the PSF kernel's extent ----
   std::atomic<double> na{1.4};
   std::atomic<double> immersionIndex{1.518};
   std::atomic<double> workingDistanceUm{150.0};
   std::atomic<double> psfKernelHalfWidthNm{7000.0};
   std::atomic<double> psfZRangeUm{7.0};
   std::atomic<double> psfZStepUm{0.1};
   std::atomic<int> psfMaskType{static_cast<int>(sim::PsfMaskType::None)};   // not exposed in MM
   std::atomic<int> psfMaskModes{5};
   std::atomic<double> psfMaskWaist{1.0};

   // ---- excitation light ----
   std::atomic<int> illumProfile{static_cast<int>(sim::IllumProfile::Flat)};
   std::atomic<double> illumFwhmPct{60.0};
   // Light sources' shutters: a missing device keeps its default (the epi
   // light on, the lamp off). lightVersion counts changes (live frames rendered
   // with other light are skipped).
   std::atomic<bool> epiOpen{true};
   std::atomic<bool> transOpen{false};
   std::atomic<long> lightVersion{0};

   // ---- sample holder ----
   std::atomic<double> backgroundPhotonsPerSec{0.0};
   std::atomic<double> bgDecaySec{0.0};
   std::atomic<double> driftXyNmPerSqrtSec{0.0};
   std::atomic<double> driftZNmPerSqrtSec{0.0};
   std::atomic<double> directedDrift[DD_COUNT];
   std::atomic<double> psfSampleIndex{1.518};
   std::atomic<double> psfSampleDepthNm{0.0};

   // ---- specimen: the cell field ----
   std::atomic<double> cellField[CF_COUNT];
   std::atomic<bool> cellFieldPacking{true};

   // ---- BrightField (lamp, render numerics, specimen indices) ----
   std::atomic<double> brightField[BF_COUNT];

   // ---- renderer ----
   std::atomic<int> psfModel{static_cast<int>(sim::PsfModelKind::GibsonLanniZernike)};
   std::atomic<int> psfOversampling{6};
   std::atomic<int> psfInterp{static_cast<int>(sim::PsfInterpMode::Cubic)};
   std::atomic<int> psfPupilSamples{0};
   std::atomic<bool> useGpu{true};
   std::atomic<int> diskCacheMode{1};                // 0 Off, 1 Cells, 2 CellsAndPsf
   std::atomic<double> wideField[WF_COUNT];

   // ---- strings (under textMutex) ----
   std::string ZernikeCoefficients() const;          // space-separated, always valid
   void SetZernikeCoefficients(const std::string& s);
   std::string ZernikePreset() const;
   void SetZernikePreset(const std::string& s);
   std::string JavaHome() const;
   void SetJavaHome(const std::string& s);
   std::string LightPreset() const;
   void SetLightPreset(const std::string& s);
   // What the camera renders on (the GPU adapter, or why the CPU).
   std::string GpuStatus() const;
   void SetGpuStatus(const std::string& s);

   // ---- the engine's scope options (labels, dyes, light path, QE curve) ----
   // option -> value; an option never set reads the engine default.
   double Option(const std::string& option) const;
   void SetOption(const std::string& option, double v);
   // Dye fields the user edited ("<prefix>.<field>" -> value); the others read the library.
   bool DyeEdit(const std::string& key, double& v) const;
   void SetDyeEdit(const std::string& key, double v);
   void ClearDyeEdit(const std::string& key);
   void ClearDyeEditsWithPrefix(const std::string& prefix);
   std::map<std::string, double> Options() const;
   std::map<std::string, double> DyeEdits() const;
   // The microtubules' effective mode at the last dye load (the camera preset's gain and the labelling suggestion follow it).
   std::atomic<int> lastMtMode{-1};

private:
   mutable std::mutex textMutex_;
   std::string zernikeCoefficients_;
   std::string zernikePreset_ = "MixedRealisticObjective";
   std::string javaHome_;
   std::string lightPreset_ = "PAINT-640";
   std::string gpuStatus_ = "Not used yet";
   mutable std::mutex optionMutex_;
   std::map<std::string, double> options_;
   std::map<std::string, double> dyeEdits_;
};

} // namespace isc
