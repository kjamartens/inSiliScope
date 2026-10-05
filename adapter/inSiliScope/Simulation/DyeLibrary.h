///////////////////////////////////////////////////////////////////////////////
// FILE:          DyeLibrary.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Dyes and their photophysics in a light path (issue 16): the
//                twin of web/prototype/scope/dye_library.js. From a dye
//                (library entry or one of three slots, with overrides), a
//                mode and the light path (LightPath.h) it gives, per
//                emitting state (main, pre): the excitation rate per dye
//                (sum over laser lines of cross section x photon flux x the
//                dichroic's reflectance), the emission rate (x QY), the
//                detected fraction (emission x dichroic T x emission filter
//                T x camera QE), the effective wavelength (the PSF's) and
//                the detected photons per second; and the world label's
//                kinetics (dSTORM times scaled by the excitation relative
//                to Dempsey et al. 2011's measurement conditions, the 405 nm
//                and primed activation, the DNA-PAINT binding rate).
//                Data: DyeLibraryData.inc, generated from data/dyes by
//                tools/gen_dye_library.mjs (never edit it by hand).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "LightPath.h"
#include "Spectra.h"

#include <limits>
#include <map>
#include <string>
#include <vector>

namespace sim {

// "Not given" in the data (JSON null or a missing field): the JS `??` falls
// back to its default where the value is NaN here.
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- the generated data's types (DyeLibraryData.inc) ----
struct SpectrumData { const char* key; const double* values; };
struct FilterData { const char* id; const char* name; const char* curve; IdealFilterSpec ideal; };
struct LightPresetData { const char* id; const char* name; double lasers[5]; const char* dichroic; const char* emissionFilter; };
enum CameraField { CAM_QE, CAM_READ_NOISE, CAM_GAIN, CAM_EM_GAIN, CAM_CIC, CAM_OFFSET, CAM_OFFSET_STD, CAM_DARK,
                   CAM_GAIN_STD_PCT, CAM_READ_NOISE_STD_PCT, CAM_BIT_DEPTH, CAM_FIELDS };
struct CameraData { const char* id; const char* name; const char* type; const char* qeCurve; double v[CAM_FIELDS]; };
struct StateData
{
   bool present;
   double exMaxNm, emMaxNm, extCoeff, qy;
   bool isDark;
   const char* ex;      // spectrum keys ("fp:<id>"), nullptr for a parametric or dark state
   const char* em;
   const char* emHex;   // emission colour
   bool parametric;
   double exPeakNm, emPeakNm, exWidthNm, emWidthNm;
};
struct PrimedData { bool present; double primeLoNm, primeHiNm, convertLoNm, convertHiNm, perKWcm2SqPerSec; };
struct ModeData
{
   bool present;
   double laser, onSec, offSec, offSecBetweenBlinks, bleachProb, photonCV, initialOnSec, activation405PerKWcm2PerSec,
      spontaneousActivationPerSec, konPerMPerSec, photonBudget, prePhotonBudget, kExcRef;
   PrimedData primed;
   const char* lightPreset;
   bool generic;
};
enum DyeMode { MODE_DSTORM = 0, MODE_PALM = 1, MODE_DNA_PAINT = 2, MODE_WIDEFIELD = 3, MODE_COUNT = 4 };   // = ISC_MODE_*
struct DyeData
{
   const char* id;
   const char* name;
   const char* defaultMode;
   double fluorescentFraction;
   StateData main, pre;
   ModeData modes[MODE_COUNT];
};

// ---- the library ----
const std::vector<std::string>& DyeIds();           // the library dyes
const std::vector<std::string>& DyeChoices();       // DyeIds() + Dye1, Dye2, Dye3 (a structure's dye)
const std::vector<std::string>& DyeModeNames();     // dSTORM, PALM, DNA-PAINT, WideField
const std::vector<std::string>& DichroicIds();
const std::vector<std::string>& EmissionFilterIds();
const std::vector<std::string>& CameraIds();
const std::vector<std::string>& LightPresetIds();
const std::vector<int>& LaserLines();               // 405, 488, 561, 640, 730
const DyeData& DyeAt(int i);
const FilterData& DichroicAt(int i);
const FilterData& EmissionFilterAt(int i);
const CameraData& CameraAt(int i);
const LightPresetData* FindLightPreset(const std::string& id);
// A sampled spectrum by key ("fp:<id>"); nullptr if unknown.
const Spectrum* SpectrumByKey(const char* key);
double SuggestedLabelingPct(int mode);
int IndexOf(const std::vector<std::string>& list, const std::string& id);   // -1 if absent
const char* DefaultDichroic();
const char* DefaultEmissionFilter();
const char* DefaultCamera();

// The override fields (`<prefix>-dye.<field>`, `dye<N>.<field>`; JS DYE_FIELDS), in their order.
const std::vector<std::string>& DyeFieldNames();

// ---- the effective dye of a structure ----
using DyeOverrides = std::map<std::string, double>;
struct DyeSlot
{
   int source = 0;   // a library dye
   DyeOverrides overrides;
};
struct EffectiveDye
{
   DyeData dye;      // the library entry with the overrides applied
   int mode = 0;     // DyeMode
};
// choice: index into DyeChoices(); modeIndex: DyeMode or -1 (the dye's
// default). The slot's overrides, then the structure's. False (with err) on
// an unknown field, a choice out of range, or a field the dye lacks.
bool MakeEffectiveDye(int choice, const std::vector<DyeSlot>& slots, const DyeOverrides& overrides, int modeIndex,
                      EffectiveDye& out, std::string& err);

// Emission and detection of one state at illumination 1.
struct StatePhysics
{
   bool emits = false;   // false: a dark state (nothing else is set)
   double excitationPerSec = 0, emissionPerSec = 0, detectedFraction = 0, lambdaNm = 0, detectedPerSec = 0;
   const char* color = nullptr;
};
bool StatePhotophysics(const StateData& st, const LightPath& lp, StatePhysics& out, std::string& err);

struct LabelPhysicsOptions
{
   double density = 0.7;     // site fraction
   double imagerNm = 0;      // DNA-PAINT imager concentration
   int orientationMode = 0;  // ISC_ORIENT_*
   double polarDeg = 90, azimuthDeg = 0, wobbleDeg = 0;
   int motion = 0;           // 0 = Static
};
// A structure's dye in its mode: the world label (isc_world_set_label
// layout) and per-state photophysics.
struct LabelPhysics
{
   EffectiveDye eff;
   int mode = 0;
   std::vector<double> label;   // ISC_LABEL_COUNT doubles
   StatePhysics main, pre;      // pre.emits only for a PALM dye with an emitting pre state
   double kActPerSec = 0, excitationScale = 1, onSecNow = 0;
   double photonBudget = 0, prePhotonBudget = 0;
   double imagerNm = 0, chamberHeightUm = 0;
   double initialOnSec = 0;     // the label's (scaled) dSTORM initial ON time
   bool preState = false;
   // The free imager's detected photons per pixel per second (DNA-PAINT):
   // c x N_A x chamber height x pixel area x its detected rate (no
   // depletion, no exclusion from cells; schnitzbauer2017).
   double ImagerBackgroundPerPxPerSec(double pixelUm) const;
};
bool MakeLabelPhysics(const EffectiveDye& eff, const LightPath& lp, const LabelPhysicsOptions& o, LabelPhysics& out,
                      std::string& err);

} // namespace sim
