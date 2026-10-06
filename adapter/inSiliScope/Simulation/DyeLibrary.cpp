///////////////////////////////////////////////////////////////////////////////
// FILE:          DyeLibrary.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See DyeLibrary.h (twin of web/prototype/scope/dye_library.js).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "DyeLibrary.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sim {

namespace {
#include "DyeLibraryData.inc"

template <class T, size_t N>
constexpr size_t Count(const T (&)[N])
{
   return N;
}

std::vector<std::string> IdsOf(const FilterData* f, size_t n)
{
   std::vector<std::string> out;
   for (size_t i = 0; i < n; ++i)
      out.push_back(f[i].id);
   return out;
}

// JS `v ?? d` for a NaN-sentinel value.
inline double Or(double v, double d) { return std::isnan(v) ? d : v; }
inline bool Given(double v) { return !std::isnan(v); }

// AVOGADRO x 1e-9 x 1e-15: molecules per um^3 at 1 nM (JS AVOGADRO_PER_NM_UM3, same operand order).
const double kAvogadroPerNmUm3 = 6.02214076e23 * 1e-9 * 1e-15;
} // namespace

const std::vector<std::string>& DyeIds()
{
   static const std::vector<std::string> ids = [] {
      std::vector<std::string> v;
      for (const DyeData& d : kDyes)
         v.push_back(d.id);
      return v;
   }();
   return ids;
}

const std::vector<std::string>& DyeChoices()
{
   static const std::vector<std::string> ids = [] {
      std::vector<std::string> v = DyeIds();
      for (const char* s : { "Dye1", "Dye2", "Dye3" })
         v.push_back(s);
      return v;
   }();
   return ids;
}

const std::vector<std::string>& DyeModeNames()
{
   static const std::vector<std::string> v = { "dSTORM", "PALM", "DNA-PAINT", "WideField" };
   return v;
}

const std::vector<std::string>& DichroicIds()
{
   static const std::vector<std::string> v = IdsOf(kDichroics, Count(kDichroics));
   return v;
}

const std::vector<std::string>& EmissionFilterIds()
{
   static const std::vector<std::string> v = IdsOf(kEmissionFilters, Count(kEmissionFilters));
   return v;
}

const std::vector<std::string>& CameraIds()
{
   static const std::vector<std::string> v = [] {
      std::vector<std::string> out;
      for (const CameraData& c : kCameras)
         out.push_back(c.id);
      return out;
   }();
   return v;
}

const std::vector<std::string>& LightPresetIds()
{
   static const std::vector<std::string> v = [] {
      std::vector<std::string> out;
      for (const LightPresetData& q : kLightPresets)
         out.push_back(q.id);
      return out;
   }();
   return v;
}

const std::vector<int>& LaserLines()
{
   static const std::vector<int> v(kLaserLines, kLaserLines + Count(kLaserLines));
   return v;
}

const DyeData& DyeAt(int i) { return kDyes[i]; }
const FilterData& DichroicAt(int i) { return kDichroics[i]; }
const FilterData& EmissionFilterAt(int i) { return kEmissionFilters[i]; }
const CameraData& CameraAt(int i) { return kCameras[i]; }

const LightPresetData* FindLightPreset(const std::string& id)
{
   for (const LightPresetData& q : kLightPresets)
      if (id == q.id)
         return &q;
   return nullptr;
}

const Spectrum* SpectrumByKey(const char* key)
{
   static const std::map<std::string, Spectrum> all = [] {
      std::map<std::string, Spectrum> m;
      for (const SpectrumData& s : kSpectra)
         m[s.key] = Spectrum(s.values, s.values + kSpectraGridN);
      return m;
   }();
   if (!key)
      return nullptr;
   auto it = all.find(key);
   return it == all.end() ? nullptr : &it->second;
}

double SuggestedLabelingPct(int mode) { return kSuggestedLabelingPct[mode]; }

int IndexOf(const std::vector<std::string>& list, const std::string& id)
{
   const auto it = std::find(list.begin(), list.end(), id);
   return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

const char* DefaultDichroic() { return kDefaultDichroic; }
const char* DefaultEmissionFilter() { return kDefaultEmissionFilter; }
const char* DefaultCamera() { return kDefaultCamera; }

const std::vector<std::string>& DyeFieldNames()
{
   static const std::vector<std::string> v = {
      "fluorescent-pct", "qy", "ext-coeff", "pre-qy", "pre-ext-coeff", "ex-peak-nm", "em-peak-nm", "ex-width-nm",
      "em-width-nm", "on-sec", "off-sec", "bleach-prob", "photon-cv", "initial-on-sec", "activation-405",
      "spont-activation", "primed", "kon", "photon-budget", "pre-photon-budget",
   };
   return v;
}

namespace {
// JS applyField.
bool ApplyField(DyeData& d, int mode, const std::string& k, double v, std::string& err)
{
   ModeData& m = d.modes[mode];
   if (k == "fluorescent-pct") { d.fluorescentFraction = v / 100; return true; }
   if (k == "qy" || k == "ext-coeff" || k == "pre-qy" || k == "pre-ext-coeff")
   {
      const bool pre = k.compare(0, 4, "pre-") == 0;
      StateData& st = pre ? d.pre : d.main;
      if (!st.present)
      {
         err = std::string(d.name) + " has no " + (pre ? "pre" : "main") + " state ('" + k + "')";
         return false;
      }
      (k == "qy" || k == "pre-qy" ? st.qy : st.extCoeff) = v;
      return true;
   }
   if (k == "ex-peak-nm" || k == "em-peak-nm" || k == "ex-width-nm" || k == "em-width-nm")
   {
      StateData& st = d.main;
      if (!st.parametric)
      {
         err = "'" + k + "' applies to parametric (Custom) dyes only";
         return false;
      }
      if (k == "ex-peak-nm") { st.exPeakNm = v; st.exMaxNm = v; }
      else if (k == "em-peak-nm") { st.emPeakNm = v; st.emMaxNm = v; }
      else if (k == "ex-width-nm") st.exWidthNm = v;
      else st.emWidthNm = v;
      return true;
   }
   if (k == "primed")
   {
      if (!m.primed.present)
         m.primed = { true, 470, 510, 690, 780, v };
      m.primed.perKWcm2SqPerSec = v;
      return true;
   }
   if (k == "on-sec") m.onSec = v;
   else if (k == "off-sec") (Given(m.offSecBetweenBlinks) ? m.offSecBetweenBlinks : m.offSec) = v;
   else if (k == "bleach-prob") m.bleachProb = v;
   else if (k == "photon-cv") m.photonCV = v;
   else if (k == "initial-on-sec") m.initialOnSec = v;
   else if (k == "activation-405") m.activation405PerKWcm2PerSec = v;
   else if (k == "spont-activation") m.spontaneousActivationPerSec = v;
   else if (k == "kon") m.konPerMPerSec = v;
   else if (k == "photon-budget") m.photonBudget = v;
   else if (k == "pre-photon-budget") m.prePhotonBudget = v;
   else
   {
      std::string all;
      for (const std::string& f : DyeFieldNames())
         all += (all.empty() ? "" : ", ") + f;
      err = "unknown dye field '" + k + "' (one of " + all + ")";
      return false;
   }
   return true;
}

bool StateSpectra(const StateData& st, Spectrum& ex, Spectrum& em, std::string& err)
{
   if (st.parametric)
   {
      ex = ParametricExcitation(st.exPeakNm, st.exWidthNm);
      em = ParametricEmission(st.emPeakNm, st.emWidthNm);
      return true;
   }
   const Spectrum* a = SpectrumByKey(st.ex);
   const Spectrum* b = SpectrumByKey(st.em);
   if (!a || !b)
   {
      err = "state without spectra";
      return false;
   }
   ex = *a;
   em = *b;
   return true;
}
} // namespace

bool MakeEffectiveDye(int choice, const std::vector<DyeSlot>& slots, const DyeOverrides& overrides, int modeIndex,
                      EffectiveDye& out, std::string& err)
{
   const int nLib = static_cast<int>(Count(kDyes));
   const DyeData* base = nullptr;
   std::vector<const DyeOverrides*> chain;
   if (choice >= nLib)
   {
      const size_t s = static_cast<size_t>(choice - nLib);
      if (s >= slots.size() || s >= 3)
      {
         err = "dye slot " + std::to_string(s + 1) + " is not set";
         return false;
      }
      if (slots[s].source < 0 || slots[s].source >= nLib)
      {
         err = "dye choice " + std::to_string(choice) + " out of range";
         return false;
      }
      base = &kDyes[slots[s].source];
      chain.push_back(&slots[s].overrides);
   }
   else if (choice >= 0)
      base = &kDyes[choice];
   if (!base)
   {
      err = "dye choice " + std::to_string(choice) + " out of range";
      return false;
   }
   chain.push_back(&overrides);
   out.dye = *base;
   if (modeIndex >= MODE_COUNT)
   {
      err = "mode " + std::to_string(modeIndex) + " out of range";
      return false;
   }
   out.mode = modeIndex >= 0 ? modeIndex : IndexOf(DyeModeNames(), out.dye.defaultMode);
   if (out.mode < 0 || !out.dye.modes[out.mode].present)
   {
      err = std::string(out.dye.name) + " has no block for that mode";
      return false;
   }
   for (const DyeOverrides* ov : chain)
      for (const auto& kv : *ov)
         if (!ApplyField(out.dye, out.mode, kv.first, kv.second, err))
            return false;
   return true;
}

bool StatePhotophysics(const StateData& st, const LightPath& lp, StatePhysics& out, std::string& err)
{
   out = StatePhysics();
   if (st.isDark)
      return true;
   Spectrum ex, em;
   if (!StateSpectra(st, ex, em, err))
      return false;
   double kExc = 0;
   for (const LaserLine& l : lp.lasers)
      kExc += CrossSectionUm2(st.extCoeff, ex, l.nm) *
              PhotonFluxPerUm2(l.kWPerCm2 * (1 - SampleAt(lp.dichroicT, l.nm)), l.nm);
   const double kEm = st.qy * kExc;
   const Detection det = Detect(em, { &lp.dichroicT, &lp.emissionT, &lp.qe });
   out.emits = true;
   out.excitationPerSec = kExc;
   out.emissionPerSec = kEm;
   out.detectedFraction = det.fraction;
   out.lambdaNm = std::isfinite(det.lambdaNm) ? det.lambdaNm : st.emMaxNm;
   out.detectedPerSec = kEm * lp.eta * det.fraction;
   out.color = st.emHex;
   return true;
}

double LabelPhysics::ImagerBackgroundPerPxPerSec(double pixelUm) const
{
   if (mode != MODE_DNA_PAINT || !main.emits)
      return 0;
   return std::max(0.0, imagerNm) * kAvogadroPerNmUm3 * chamberHeightUm * pixelUm * pixelUm * main.detectedPerSec;
}

bool MakeLabelPhysics(const EffectiveDye& eff, const LightPath& lp, const LabelPhysicsOptions& o, LabelPhysics& out,
                      std::string& err)
{
   out = LabelPhysics();
   out.eff = eff;
   const DyeData& dye = out.eff.dye;
   const int mode = eff.mode;
   const ModeData& m = dye.modes[mode];
   out.mode = mode;
   if (!StatePhotophysics(dye.main, lp, out.main, err))
      return false;
   if (mode == MODE_PALM && dye.pre.present && !StatePhotophysics(dye.pre, lp, out.pre, err))
      return false;
   // kinetics: activationRatePerSec, onSec, offSec, bleachProb, photonCV, initialOnSec
   double act = 0, onSec = std::max(1e-6, Or(m.onSec, 0.05)), offSec = 1.0, bleachProb = 1.0;
   const double photonCV = std::max(0.0, Or(m.photonCV, 0.5));
   double initialOnSec = 0;
   double kAct = 0, excitationScale = 1;
   if (mode == MODE_DSTORM)
   {
      // ON time, spontaneous dark time and initial ON time are given at the
      // reference light path (Dempsey et al. 2011's measurement intensity)
      // and scale as 1 / (excitation rate / kExcRef); the 405 nm activation
      // adds to the return rate (dye_library.js labelPhotophysics).
      excitationScale = out.main.emits && m.kExcRef > 0 ? std::max(1e-6, out.main.excitationPerSec / m.kExcRef) : 1;
      onSec = std::max(1e-6, Or(m.onSec, 0.02) / excitationScale);
      kAct = excitationScale / std::max(1e-6, m.offSec) +
             Or(m.activation405PerKWcm2PerSec, 0) * LaserIntensityAt(lp, 405);
      act = kAct;
      offSec = 1 / kAct;
      bleachProb = m.bleachProb;
      initialOnSec = Or(m.initialOnSec, 0) / excitationScale;
   }
   else if (mode == MODE_PALM)
   {
      kAct = Or(m.spontaneousActivationPerSec, 0) + Or(m.activation405PerKWcm2PerSec, 0) * LaserIntensityAt(lp, 405);
      if (m.primed.present)
         kAct += m.primed.perKWcm2SqPerSec * LaserIntensityIn(lp, m.primed.primeLoNm, m.primed.primeHiNm) *
                 LaserIntensityIn(lp, m.primed.convertLoNm, m.primed.convertHiNm);
      act = kAct;
      offSec = Or(m.offSecBetweenBlinks, 0.5);
      bleachProb = Or(m.bleachProb, 1);
   }
   else if (mode == MODE_DNA_PAINT)
   {
      kAct = Or(m.konPerMPerSec, 1e6) * std::max(0.0, o.imagerNm) * 1e-9;
      act = kAct;
   }
   if (o.motion != 0)
   {
      err = "motion is not implemented yet (only Static)";
      return false;
   }
   if (o.orientationMode < 0 || o.orientationMode > 2)
   {
      err = "orientation is not one of Free, Fixed, Random";
      return false;
   }
   out.preState = out.pre.emits || (mode == MODE_PALM && dye.pre.present);
   out.label.assign(ISC_LABEL_COUNT, 0.0);
   out.label[ISC_LABEL_DENSITY] = o.density;
   out.label[ISC_LABEL_FLUORESCENT_FRACTION] = dye.fluorescentFraction;
   out.label[ISC_LABEL_MODE] = mode;
   out.label[ISC_LABEL_ACTIVATION_RATE] = act;
   out.label[ISC_LABEL_ON_SEC] = onSec;
   out.label[ISC_LABEL_OFF_SEC] = offSec;
   out.label[ISC_LABEL_BLEACH_PROB] = bleachProb;
   out.label[ISC_LABEL_PHOTON_CV] = photonCV;
   out.label[ISC_LABEL_INITIAL_ON_SEC] = initialOnSec;
   out.label[ISC_LABEL_PRE_STATE] = out.preState ? 1 : 0;
   out.label[ISC_LABEL_ORIENT_MODE] = o.orientationMode;
   out.label[ISC_LABEL_ORIENT_POLAR_DEG] = o.polarDeg;
   out.label[ISC_LABEL_ORIENT_AZIMUTH_DEG] = o.azimuthDeg;
   out.label[ISC_LABEL_WOBBLE_DEG] = o.wobbleDeg;
   out.kActPerSec = kAct;
   out.excitationScale = excitationScale;
   out.onSecNow = onSec;
   out.initialOnSec = initialOnSec;
   out.photonBudget = Or(m.photonBudget, mode == MODE_WIDEFIELD ? 100000 : 0);
   out.prePhotonBudget = Or(m.prePhotonBudget, 0);
   out.imagerNm = o.imagerNm;
   out.chamberHeightUm = lp.chamberHeightUm;
   return true;
}

} // namespace sim
