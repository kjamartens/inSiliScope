///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeProperties.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Issue 16: the camera's structures / dyes / light path
//                properties. Most are one engine scope option each
//                (Simulation/ScopeMovie.h, the cli/viewer's options: the
//                microtubules' dye, mode, labelling, orientation; the dye
//                slots; lasers, dichroic, emission filter; the camera's QE
//                curve; the mean-field switch). The dye fields
//                (FluoParam_Microtubule_*, FluoParam_Dye<N>_*) load from the
//                dye library on a dye pick or mode change and stay the
//                library's exact values until edited. BuildScopeSpec turns
//                every property into the spec FluorescenceMovie renders.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"

#include "Simulation/SharedStageState.h"

#include "Simulation/DyeLibrary.h"
#include "Simulation/SMLMZernike.h"
#include "Simulation/Spectra.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace {

// The 14 dye fields the viewer shows (web/index.html DYE_FIELD_ROWS), the
// modes each applies to (empty: all), and their property name suffixes.
struct FieldRow
{
   const char* field;
   const char* suffix;
   std::vector<int> modes;
};
const std::vector<FieldRow>& FieldRows()
{
   using namespace sim;
   static const std::vector<FieldRow> rows = {
      { "fluorescent-pct", "FluorescentPct", {} },
      { "qy", "Qy", {} },
      { "ext-coeff", "ExtCoeff", {} },
      { "on-sec", "OnSec", { MODE_DSTORM, MODE_PALM, MODE_DNA_PAINT } },
      { "off-sec", "OffSec", { MODE_DSTORM, MODE_PALM } },
      { "bleach-prob", "BleachProb", { MODE_DSTORM, MODE_PALM } },
      { "initial-on-sec", "InitialOnSec", { MODE_DSTORM } },
      { "activation-405", "Activation405", { MODE_DSTORM, MODE_PALM } },
      { "spont-activation", "SpontActivation", { MODE_PALM } },
      { "primed", "Primed", { MODE_PALM } },
      { "pre-photon-budget", "PrePhotonBudget", { MODE_PALM } },
      { "kon", "Kon", { MODE_DNA_PAINT } },
      { "photon-cv", "PhotonCv", { MODE_DSTORM, MODE_PALM, MODE_DNA_PAINT } },
      { "photon-budget", "PhotonBudget", { MODE_WIDEFIELD } },
   };
   return rows;
}

// A dye's field value in a mode (the viewer's getters); NaN where the field
// does not apply.
double FieldValue(const sim::DyeData& d, int mode, const FieldRow& r)
{
   if (!r.modes.empty() && std::find(r.modes.begin(), r.modes.end(), mode) == r.modes.end())
      return sim::kNaN;
   const sim::ModeData& m = d.modes[mode];
   const std::string f = r.field;
   if (f == "fluorescent-pct") return 100 * d.fluorescentFraction;
   if (f == "qy") return d.main.qy;
   if (f == "ext-coeff") return d.main.extCoeff;
   if (f == "on-sec") return m.onSec;
   if (f == "off-sec") return mode == sim::MODE_PALM ? m.offSecBetweenBlinks : m.offSec;
   if (f == "bleach-prob") return m.bleachProb;
   if (f == "initial-on-sec") return m.initialOnSec;
   if (f == "activation-405") return m.activation405PerKWcm2PerSec;
   if (f == "spont-activation") return m.spontaneousActivationPerSec;
   if (f == "primed") return m.primed.present ? m.primed.perKWcm2SqPerSec : 0.0;
   if (f == "pre-photon-budget") return std::isnan(m.prePhotonBudget) ? 0.0 : m.prePhotonBudget;
   if (f == "kon") return m.konPerMPerSec;
   if (f == "photon-cv") return m.photonCV;
   if (f == "photon-budget") return m.photonBudget;
   return sim::kNaN;
}

std::string Num(double v)
{
   std::ostringstream o;
   o.precision(10);
   o << v;
   return o.str();
}

} // namespace

double CInSiliScopeCamera::OptionValue(const std::string& option) const
{
   std::lock_guard<std::mutex> g(optionMutex_);
   auto it = option_.find(option);
   return it != option_.end() ? it->second : sim::ScopeSpecGet(sim::ScopeSpec(), option.c_str());
}

void CInSiliScopeCamera::SetOptionValue(const std::string& option, double v)
{
   std::lock_guard<std::mutex> g(optionMutex_);
   option_[option] = v;
}

void CInSiliScopeCamera::NotifyOption(const std::string& option)
{
   for (const OptionProp& o : optionProps_)
      if (o.option == option)
      {
         const double v = OptionValue(option);
         if (o.kind == 2)
         {
            const int i = static_cast<int>(v) - o.offset;
            if (i >= 0 && i < static_cast<int>(o.names.size()))
               OnPropertyChanged(o.prop.c_str(), o.names[static_cast<size_t>(i)].c_str());
         }
         else
            OnPropertyChanged(o.prop.c_str(), Num(v).c_str());
      }
}

void CInSiliScopeCamera::CreateScopeProperties()
{
   using namespace sim;
   std::vector<std::string> modeNames = { "DyeDefault" };
   for (const std::string& m : DyeModeNames())
      modeNames.push_back(m);
   auto F = [](const char* prop, const char* option, double lo, double hi) {
      OptionProp o;
      o.prop = prop;
      o.option = option;
      o.kind = 0;
      o.lo = lo;
      o.hi = hi;
      return o;
   };
   auto E = [](const char* prop, const char* option, const std::vector<std::string>& names, int offset = 0) {
      OptionProp o;
      o.prop = prop;
      o.option = option;
      o.kind = 2;
      o.names = names;
      o.offset = offset;
      return o;
   };
   optionProps_ = {
      E("SimType_CellFieldMicrotubuleDye", "mt-dye", DyeChoices()),
      E("SimType_CellFieldMicrotubuleLabelMode", "mt-mode", modeNames, -1),
      F("SimType_CellFieldMicrotubuleLabelingPct", "mt-label-pct", 0, 100),
      F("SimType_CellFieldMicrotubuleImagerNm", "mt-imager-nm", 0, 10000),
      E("SimType_CellFieldMicrotubuleOrientation", "mt-orient", { "Free", "Fixed", "Random" }),
      F("SimType_CellFieldMicrotubuleOrientPolarDeg", "mt-orient-polar-deg", 0, 180),
      F("SimType_CellFieldMicrotubuleOrientAzimuthDeg", "mt-orient-azimuth-deg", -360, 360),
      F("SimType_CellFieldMicrotubuleWobbleConeDeg", "mt-wobble-deg", 0, 90),
      E("SimType_CellFieldMicrotubuleMotion", "mt-motion", { "Static" }),
      E("FluoParam_Dye1_Source", "dye1.source", DyeIds()),
      E("FluoParam_Dye2_Source", "dye2.source", DyeIds()),
      E("FluoParam_Dye3_Source", "dye3.source", DyeIds()),
   };
   for (int nm : LaserLines())
   {
      OptionProp o = F("", "", 0, 100);
      o.prop = "Optics_Laser" + std::to_string(nm) + "KWcm2";
      o.option = "laser-" + std::to_string(nm);
      optionProps_.push_back(o);
   }
   const std::vector<OptionProp> rest = {
      F("Optics_LaserCustomNm", "laser-custom-nm", 0, 1000),
      F("Optics_LaserCustomKWcm2", "laser-custom", 0, 100),
      E("Optics_IlluminationGeometry", "illum-geometry", { "Epi" }),
      F("Optics_ChamberHeightUm", "chamber-height-um", 0, 1000),
      E("Optics_Dichroic", "dichroic", DichroicIds()),
      F("Optics_DichroicEdgeNm", "dichroic-edge-nm", 300, 900),
      E("Optics_EmissionFilter", "em-filter", EmissionFilterIds()),
      F("Optics_EmissionLoNm", "em-lo-nm", 300, 900),
      F("Optics_EmissionHiNm", "em-hi-nm", 300, 900),
      E("CamParam_QeCurve", "qe-curve", CameraIds()),
      F("General_MeanFieldDensityPerUm2", "mean-field-density-per-um2", 0, 1e6),
      F("General_MeanFieldSlabNm", "mean-field-slab-nm", 0, 10000),
      F("General_MeanFieldMaxEmitters", "mean-field-max-emitters", 0, 1e9),
   };
   optionProps_.insert(optionProps_.end(), rest.begin(), rest.end());
   // Defaults: the engine's (ScopeMovieOptions), the labelling at the default
   // mode's suggestion, the QE curve of the default camera.
   {
      std::lock_guard<std::mutex> g(optionMutex_);
      for (const OptionProp& o : optionProps_)
         option_[o.option] = ScopeSpecGet(ScopeSpec(), o.option.c_str());
      option_["mt-label-pct"] = SuggestedLabelingPct(IndexOf(DyeModeNames(), DyeAt(static_cast<int>(option_["mt-dye"])).defaultMode));
      option_["qe-curve"] = IndexOf(CameraIds(), DefaultCamera());
   }
   cameraPreset_ = IndexOf(CameraIds(), DefaultCamera());
   lastMtMode_ = IndexOf(DyeModeNames(), DyeAt(static_cast<int>(OptionValue("mt-dye"))).defaultMode);
   for (size_t i = 0; i < optionProps_.size(); ++i)
   {
      const OptionProp& o = optionProps_[i];
      auto* act = new CPropertyActionEx(this, &CInSiliScopeCamera::OnScopeOption, static_cast<long>(i));
      const double v = OptionValue(o.option);
      if (o.kind == 2)
      {
         CreateStringProperty(o.prop.c_str(), o.names[static_cast<size_t>(static_cast<int>(v) - o.offset)].c_str(), false, act);
         for (const std::string& n : o.names)
            AddAllowedValue(o.prop.c_str(), n.c_str());
      }
      else
      {
         CreateFloatProperty(o.prop.c_str(), v, false, act);
         SetPropertyLimits(o.prop.c_str(), o.lo, o.hi);
      }
   }
   // The dye fields: the microtubules' dye and the three slots.
   dyeFieldProps_.clear();
   for (const char* prefix : { "mt-dye", "dye1", "dye2", "dye3" })
      for (const FieldRow& r : FieldRows())
      {
         DyeFieldProp d;
         d.prefix = prefix;
         d.field = r.field;
         d.prop = std::string(!std::strcmp(prefix, "mt-dye") ? "FluoParam_Microtubule_"
                                                             : std::string("FluoParam_Dye") + prefix[3] + "_") +
                  r.suffix;
         dyeFieldProps_.push_back(d);
      }
   for (size_t i = 0; i < dyeFieldProps_.size(); ++i)
      CreateFloatProperty(dyeFieldProps_[i].prop.c_str(), 0.0, false,
                          new CPropertyActionEx(this, &CInSiliScopeCamera::OnDyeField, static_cast<long>(i)));
   // Light preset, camera preset, read-only label readouts.
   CreateStringProperty("Optics_Preset", lightPreset_.c_str(), false,
                        new CPropertyAction(this, &CInSiliScopeCamera::OnLightPreset));
   AddAllowedValue("Optics_Preset", "None");
   for (const std::string& id : LightPresetIds())
      AddAllowedValue("Optics_Preset", id.c_str());
   CreateStringProperty("CamParam_CameraPreset", CameraIds()[static_cast<size_t>(cameraPreset_)].c_str(), false,
                        new CPropertyAction(this, &CInSiliScopeCamera::OnCameraPreset));
   for (const std::string& id : CameraIds())
      AddAllowedValue("CamParam_CameraPreset", id.c_str());
   const char* readouts[] = { "FluoParam_Microtubule_DetectedPct", "FluoParam_Microtubule_EffectiveEmissionNm",
                              "FluoParam_Microtubule_PhotonsPerSecOn" };
   for (long i = 0; i < 3; ++i)
      CreateFloatProperty(readouts[i], 0.0, true,
                          new CPropertyActionEx(this, &CInSiliScopeCamera::OnLabelReadout, i));
}

int CInSiliScopeCamera::OnScopeOption(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   if (index < 0 || index >= static_cast<long>(optionProps_.size()))
      return DEVICE_INVALID_PROPERTY;
   const OptionProp& o = optionProps_[static_cast<size_t>(index)];
   if (eAct == MM::BeforeGet)
   {
      const double v = OptionValue(o.option);
      if (o.kind == 2)
      {
         const int i = static_cast<int>(v) - o.offset;
         if (i >= 0 && i < static_cast<int>(o.names.size()))
            pProp->Set(o.names[static_cast<size_t>(i)].c_str());
      }
      else
         pProp->Set(v);
   }
   else if (eAct == MM::AfterSet)
   {
      double v = 0;
      if (o.kind == 2)
      {
         std::string s;
         pProp->Get(s);
         const auto it = std::find(o.names.begin(), o.names.end(), s);
         if (it == o.names.end())
            return DEVICE_INVALID_PROPERTY_VALUE;
         v = static_cast<double>(it - o.names.begin()) + o.offset;
      }
      else
         pProp->Get(v);
      const double old = OptionValue(o.option);
      SetOptionValue(o.option, v);
      if (v != old)
      {
         if (o.option == "mt-dye" || o.option == "mt-mode")
            LoadMicrotubuleDye(true);
         else if (o.option.compare(0, 3, "dye") == 0 && o.option.size() > 5 && o.option.substr(4) == ".source")
         {
            // A slot's new source: its own edits go; the microtubules reload if they use it.
            const std::string prefix = o.option.substr(0, 4);
            {
               std::lock_guard<std::mutex> g(optionMutex_);
               for (auto it = dyeEdited_.begin(); it != dyeEdited_.end();)
                  it = it->first.compare(0, prefix.size() + 1, prefix + ".") == 0 ? dyeEdited_.erase(it) : std::next(it);
            }
            const int slot = prefix[3] - '1';
            if (static_cast<int>(OptionValue("mt-dye")) == static_cast<int>(sim::DyeIds().size()) + slot)
               LoadMicrotubuleDye(true);
         }
         else if (o.option.compare(0, 6, "laser-") == 0 || o.option == "dichroic" || o.option == "em-filter")
         {
            lightPreset_ = "None";   // the light path no longer is a preset's
            OnPropertyChanged("Optics_Preset", lightPreset_.c_str());
         }
      }
      InvalidateStack();
   }
   return DEVICE_OK;
}

void CInSiliScopeCamera::LoadMicrotubuleDye(bool modeMayChange)
{
   using namespace sim;
   // The edits of the microtubules' dye fields go: they reload from the library.
   {
      std::lock_guard<std::mutex> g(optionMutex_);
      for (auto it = dyeEdited_.begin(); it != dyeEdited_.end();)
         it = it->first.compare(0, 7, "mt-dye.") == 0 ? dyeEdited_.erase(it) : std::next(it);
   }
   std::vector<DyeSlot> slots(3);
   for (int n = 0; n < 3; ++n)
      slots[static_cast<size_t>(n)].source = static_cast<int>(OptionValue("dye" + std::to_string(n + 1) + ".source"));
   EffectiveDye eff;
   std::string err;
   if (!MakeEffectiveDye(static_cast<int>(OptionValue("mt-dye")), slots, {}, static_cast<int>(OptionValue("mt-mode")), eff,
                         err))
   {
      LogMessage("Microtubule dye: " + err, false);
      return;
   }
   const bool modeChanged = eff.mode != lastMtMode_;
   if (modeMayChange && modeChanged)
   {
      SetOptionValue("mt-label-pct", SuggestedLabelingPct(eff.mode));
      NotifyOption("mt-label-pct");
   }
   lastMtMode_ = eff.mode;
   if (modeChanged)
      ApplyModeGain();
   if (eff.dye.modes[eff.mode].lightPreset)
      ApplyLightPreset(eff.dye.modes[eff.mode].lightPreset);
   for (const DyeFieldProp& d : dyeFieldProps_)
      if (d.prefix == "mt-dye")
      {
         for (const FieldRow& r : FieldRows())
            if (d.field == r.field)
            {
               const double v = FieldValue(eff.dye, eff.mode, r);
               OnPropertyChanged(d.prop.c_str(), Num(std::isnan(v) ? 0.0 : v).c_str());
            }
      }
}

void CInSiliScopeCamera::ApplyLightPreset(const std::string& id)
{
   const sim::LightPresetData* q = sim::FindLightPreset(id);
   if (!q)
      return;
   const std::vector<int>& lines = sim::LaserLines();
   for (size_t l = 0; l < lines.size(); ++l)
   {
      SetOptionValue("laser-" + std::to_string(lines[l]), q->lasers[l]);
      NotifyOption("laser-" + std::to_string(lines[l]));
   }
   SetOptionValue("dichroic", sim::IndexOf(sim::DichroicIds(), q->dichroic));
   SetOptionValue("em-filter", sim::IndexOf(sim::EmissionFilterIds(), q->emissionFilter));
   NotifyOption("dichroic");
   NotifyOption("em-filter");
   lightPreset_ = id;
   OnPropertyChanged("Optics_Preset", lightPreset_.c_str());
}

int CInSiliScopeCamera::OnLightPreset(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(lightPreset_.c_str());
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      if (s == "None")
         lightPreset_ = s;
      else
         ApplyLightPreset(s);
      InvalidateStack();
   }
   return DEVICE_OK;
}

void CInSiliScopeCamera::ApplyCameraPreset(int index)
{
   if (index < 0 || index >= static_cast<int>(sim::CameraIds().size()))
      return;
   const sim::CameraData& c = sim::CameraAt(index);
   cameraPreset_ = index;
   auto set = [&](std::atomic<double>& member, sim::CameraField f, const char* prop) {
      if (!std::isnan(c.v[f]))
      {
         member = c.v[f];
         OnPropertyChanged(prop, Num(c.v[f]).c_str());
      }
   };
   set(quantumEfficiency_, sim::CAM_QE, g_PropQuantumEfficiency);
   set(readNoiseElectrons_, sim::CAM_READ_NOISE, g_PropReadNoise);
   set(cicElectrons_, sim::CAM_CIC, g_PropCicElectrons);
   set(offsetAdu_, sim::CAM_OFFSET, g_PropOffset);
   set(offsetStdAdu_, sim::CAM_OFFSET_STD, g_PropOffsetStd);
   set(darkCurrentPerSec_, sim::CAM_DARK, g_PropDarkCurrentPerSec);
   set(pixelGainStdPct_, sim::CAM_GAIN_STD_PCT, g_PropPixelGainStdPct);
   set(pixelReadNoiseStdPct_, sim::CAM_READ_NOISE_STD_PCT, g_PropPixelReadNoiseStdPct);
   if (!std::isnan(c.v[sim::CAM_BIT_DEPTH]))
   {
      bitDepth_ = static_cast<int>(c.v[sim::CAM_BIT_DEPTH]);
      OnPropertyChanged(g_PropBitDepth, std::to_string(bitDepth_).c_str());
   }
   if (c.type)
   {
      cameraEmccd_ = !std::strcmp(c.type, "EMCCD");
      OnPropertyChanged(g_PropCameraType, cameraEmccd_ ? g_CameraTypeEmccd : g_CameraTypeScmos);
   }
   SetOptionValue("qe-curve", index);
   NotifyOption("qe-curve");
   const double gain = sim::CameraPresetGain(c, modality_.load() == 1 || lastMtMode_ == sim::IndexOf(sim::DyeModeNames(), "WideField"));
   if (!std::isnan(gain))
   {
      gainPhotonsPerAdu_ = gain;
      OnPropertyChanged(g_PropGain, Num(gain).c_str());
   }
   OnPropertyChanged(g_PropEmGain, Num(EmGain()).c_str());   // the preset's pre-amplifier sensitivity / the gain
}

// A preset whose gain depends on the imaging (CAM_GAIN_WF: the EMCCD's single-molecule vs WideField/BrightField gain)
// re-applies it when the microtubules' mode or the modality changes; other presets leave an edited gain alone.
void CInSiliScopeCamera::ApplyModeGain()
{
   if (cameraPreset_ < 0 || cameraPreset_ >= static_cast<int>(sim::CameraIds().size()))
      return;
   const sim::CameraData& c = sim::CameraAt(cameraPreset_);
   if (std::isnan(c.v[sim::CAM_GAIN_WF]))
      return;
   const double gain = sim::CameraPresetGain(c, modality_.load() == 1 || lastMtMode_ == sim::IndexOf(sim::DyeModeNames(), "WideField"));
   gainPhotonsPerAdu_ = gain;
   OnPropertyChanged(g_PropGain, Num(gain).c_str());
   OnPropertyChanged(g_PropEmGain, Num(EmGain()).c_str());
}

int CInSiliScopeCamera::OnCameraPreset(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(sim::CameraIds()[static_cast<size_t>(cameraPreset_)].c_str());
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      ApplyCameraPreset(sim::IndexOf(sim::CameraIds(), s));
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnDyeField(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   using namespace sim;
   if (index < 0 || index >= static_cast<long>(dyeFieldProps_.size()))
      return DEVICE_INVALID_PROPERTY;
   const DyeFieldProp& d = dyeFieldProps_[static_cast<size_t>(index)];
   const std::string key = d.prefix + "." + d.field;
   const FieldRow* row = nullptr;
   for (const FieldRow& r : FieldRows())
      if (d.field == r.field)
         row = &r;
   // The library value: the microtubules' effective dye in its mode, or the
   // slot's source dye in its default mode.
   double lib = kNaN;
   {
      std::vector<DyeSlot> slots(3);
      for (int n = 0; n < 3; ++n)
         slots[static_cast<size_t>(n)].source = static_cast<int>(OptionValue("dye" + std::to_string(n + 1) + ".source"));
      EffectiveDye eff;
      std::string err;
      bool ok;
      if (d.prefix == "mt-dye")
         ok = MakeEffectiveDye(static_cast<int>(OptionValue("mt-dye")), slots, {}, static_cast<int>(OptionValue("mt-mode")),
                               eff, err);
      else
         ok = MakeEffectiveDye(slots[static_cast<size_t>(d.prefix[3] - '1')].source, slots, {}, -1, eff, err);
      if (ok && row)
         lib = FieldValue(eff.dye, eff.mode, *row);
   }
   if (eAct == MM::BeforeGet)
   {
      std::lock_guard<std::mutex> g(optionMutex_);
      auto it = dyeEdited_.find(key);
      pProp->Set(it != dyeEdited_.end() ? it->second : (std::isnan(lib) ? 0.0 : lib));
   }
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      if (std::isnan(lib))
         return DEVICE_OK;   // not a field of this dye in this mode: nothing to override
      {
         std::lock_guard<std::mutex> g(optionMutex_);
         // MM keeps a few digits only: a value that reads back as the
         // library's is the library's.
         if (std::fabs(v - lib) <= 1e-6 * std::max(1.0, std::fabs(lib)) || Num(v) == Num(lib))
            dyeEdited_.erase(key);
         else
            dyeEdited_[key] = v;
      }
      InvalidateStack();
   }
   return DEVICE_OK;
}

int CInSiliScopeCamera::OnLabelReadout(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   if (eAct == MM::BeforeGet)
   {
      double x = 0, y = 0, z = 0;
      sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), x, y);
      sim::ScopeStateReadout r;
      std::string err;
      if (sim::ScopeLabelState(BuildScopeSpec(x, y, z, 60, 1), 0, false, r, err) && r.emits)
         pProp->Set(index == 0 ? 100 * r.detectedFraction : index == 1 ? r.lambdaNm : r.detectedPerSec);
      else
         pProp->Set(0.0);
   }
   return DEVICE_OK;
}

double CInSiliScopeCamera::BrightFieldQe() const
{
   const int qc = static_cast<int>(OptionValue("qe-curve"));
   if (qc >= 0 && qc < static_cast<int>(sim::CameraIds().size()))
      if (const sim::Spectrum* s = sim::SpectrumByKey(sim::CameraAt(qc).qeCurve))
         return sim::SampleAt(*s, brightFieldNum_[BF_WAVELENGTH_NM].load());
   return quantumEfficiency_.load();
}

sim::ScopeSpec CInSiliScopeCamera::BuildScopeSpec(double stageXUm, double stageYUm, double zStageUm, double startSec,
                                                  long frames) const
{
   sim::ScopeSpec s;
   s["seed"] = static_cast<double>(randomSeed_);
   s["disk-cache"] = diskCacheMode_.load();
   s["x"] = stageXUm;
   s["y"] = stageYUm;
   s["z"] = zStageUm;
   s["size"] = FullWidth();
   s["frames"] = static_cast<double>(frames);
   s["exposure-ms"] = GetExposure();
   s["start-sec"] = startSec;
   s["pixel-nm"] = pixelSizeNm_.load();
   s["background-per-sec"] = backgroundPhotonsPerSec_.load();
   s["na"] = psfNa_.load();
   s["focus-um"] = cellField_[CF_FOCUS_HEIGHT_UM].load();
   s["z-range-um"] = cellField_[CF_Z_RANGE_UM].load();
   {
      std::lock_guard<std::mutex> g(optionMutex_);
      for (const auto& kv : option_)
         s[kv.first] = kv.second;
      for (const auto& kv : dyeEdited_)
         s[kv.first] = kv.second;
   }
   // The light path and camera are the properties' (no preset of the engine's own).
   s["light-preset"] = -1;
   s["camera-preset"] = cameraPreset_;
   s["qe"] = quantumEfficiency_.load();
   s["camera-type"] = cameraEmccd_ ? 1 : 0;
   s["dark-per-sec"] = darkCurrentPerSec_.load();
   s["gain"] = gainPhotonsPerAdu_.load();
   s["offset"] = offsetAdu_.load();
   s["offset-std"] = offsetStdAdu_.load();
   s["read-noise"] = readNoiseElectrons_.load();
   s["gain-std-pct"] = pixelGainStdPct_.load();
   s["read-noise-std-pct"] = pixelReadNoiseStdPct_.load();
   s["em-gain"] = EmGain();
   s["cic"] = cicElectrons_.load();
   s["bit-depth"] = bitDepth_;
   // The cell field.
   s["chunk-um"] = cellField_[CF_CHUNK_SIZE_UM].load();
   s["occupancy"] = cellField_[CF_OCCUPANCY].load();
   s["cell-diam-min-um"] = cellField_[CF_CELL_DIAM_MIN_UM].load();
   s["cell-diam-max-um"] = cellField_[CF_CELL_DIAM_MAX_UM].load();
   s["mt-density"] = cellField_[CF_MT_DENSITY].load();
   s["packing"] = cellFieldPacking_ ? 1 : 0;
   for (int i = 0; i < CF_COUNT; ++i)
      if (g_CellFieldCoreParam[i])
         s[std::string("p.") + g_CellFieldCoreParam[i]] = cellField_[i].load();
   // Imaging.
   s["modality"] = modality_.load();
   s["wf-upscale"] = wideFieldNum_[WF_UPSCALING].load();
   s["wf-plane-nm"] = wideFieldNum_[WF_Z_PLANE_NM].load();
   s["wf-kernel-um"] = std::max(0.1, psfKernelHalfWidthNm_.load() / 1000.0);
   // The PSF: the coefficients as zern.* over the None preset.
   s["immersion-index"] = psfImmersionIndex_.load();
   s["psf-model"] = psfModel_;
   s["psf-zernike-preset"] = sim::IndexOf(sim::ZernikePresetNames(), "None");
   {
      bool ok = false;
      const sim::ZernikeCoefficients z = sim::ParseZernikeCoefficients(psfZernikeCoefficients_, ok);
      for (size_t j = 0; j < z.size(); ++j)
         s["zern." + std::to_string(j)] = z[j];
   }
   s["psf-mask"] = psfMaskType_;
   s["psf-mask-modes"] = psfMaskModes_;
   s["psf-mask-waist"] = psfMaskWaist_.load();
   s["psf-oversampling"] = psfOversampling_;
   s["psf-kernel-half-width-nm"] = psfKernelHalfWidthNm_.load();
   s["psf-z-range-um"] = psfZRangeUm_.load();
   s["psf-z-step-um"] = psfZStepUm_.load();
   s["psf-sample-index"] = psfSampleIndex_.load();
   s["psf-working-distance-um"] = psfWorkingDistanceUm_.load();
   s["psf-sample-depth-nm"] = psfSampleDepthNm_.load();
   s["psf-interp"] = psfInterp_;
   return s;
}
