///////////////////////////////////////////////////////////////////////////////
// FILE:          PropertyTable.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The property registry (PropertyTable.h): every property of
//                every inSiliScope device, grouped by device, in creation
//                order. A new property is one row here: pick its device and
//                tier by spec/MM_DEVICES.md, bind it to a setting of
//                SceneState (or an engine option), give it a one-line help.
//                Not here: the pre-init properties (the hub's Detail and
//                RandomSeed, the camera's FovSize), MM's standard ones
//                (Exposure, Binning, State, Label, Position, ...) and the
//                camera's own acquisition readouts and Test rows -- those are
//                the devices' own (InSiliScopeHub.cpp, InSiliScopeCamera.cpp).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "PropertyTable.h"

#include "../InSiliScopeHub.h"
#include "../Simulation/CacheDir.h"
#include "../Simulation/DyeLibrary.h"
#include "../Simulation/Excitation.h"
#include "../Simulation/RenderPresets.h"
#include "../Simulation/ScopeMovie.h"
#include "../Simulation/SMLMZernike.h"
#include "SceneSettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace isc {

const char* TierName(Tier t)
{
   switch (t)
   {
   case Tier::Basic: return "Basic";
   case Tier::Advanced: return "Advanced";
   case Tier::Expert: return "Expert";
   case Tier::Test: return "Test";
   }
   return "Basic";
}

Tier TierFromName(const std::string& s)
{
   if (s == "Advanced") return Tier::Advanced;
   if (s == "Expert") return Tier::Expert;
   return Tier::Basic;
}

std::string FormatNumber(double v)
{
   std::ostringstream o;
   o.precision(10);
   o << v;
   return o.str();
}

namespace {

using H = InSiliScopeHub;
using AtomD = std::atomic<double> SceneState::*;
using AtomI = std::atomic<int> SceneState::*;

// ---- row builders ----

PropDef Row(const char* device, const std::string& name, Tier tier, const std::string& key, const char* help)
{
   PropDef d;
   d.device = device;
   d.name = name;
   d.tier = tier;
   d.key = key;
   d.help = help;
   return d;
}

// A number bound to a SceneState atomic.
PropDef Num(const char* device, const std::string& name, Tier tier, const std::string& key, AtomD m, double lo,
            double hi, const char* help)
{
   PropDef d = Row(device, name, tier, key, help);
   d.lo = lo;
   d.hi = hi;
   d.get = [m](H& h) { return (h.State().*m).load(); };
   d.set = [m](H& h, double v) { (h.State().*m).store(v); };
   return d;
}

// An integer bound to a SceneState atomic<int>.
PropDef Int(const char* device, const std::string& name, Tier tier, const std::string& key, AtomI m, double lo,
            double hi, const char* help)
{
   PropDef d = Row(device, name, tier, key, help);
   d.kind = PropKind::Integer;
   d.lo = lo;
   d.hi = hi;
   d.get = [m](H& h) { return static_cast<double>((h.State().*m).load()); };
   d.set = [m, lo, hi](H& h, double v) { (h.State().*m).store(static_cast<int>(std::lround(std::min(hi, std::max(lo, v))))); };
   return d;
}

// A number of one of SceneState's arrays.
template <size_t N>
PropDef Arr(const char* device, const std::string& name, Tier tier, const std::string& key,
            std::atomic<double> (SceneState::*arr)[N], int index, double lo, double hi, const char* help,
            bool integer = false)
{
   PropDef d = Row(device, name, tier, key, help);
   d.kind = integer ? PropKind::Integer : PropKind::Float;
   d.lo = lo;
   d.hi = hi;
   d.get = [arr, index](H& h) { return (h.State().*arr)[index].load(); };
   d.set = [arr, index](H& h, double v) { (h.State().*arr)[index].store(v); };
   return d;
}

// One of named values bound to a SceneState atomic<int> (value = index).
PropDef Names(const char* device, const std::string& name, Tier tier, const std::string& key, AtomI m,
              const std::vector<std::string>& names, const char* help)
{
   PropDef d = Row(device, name, tier, key, help);
   d.kind = PropKind::Text;
   d.getText = [m, names](H& h) {
      const int i = (h.State().*m).load();
      return i >= 0 && i < static_cast<int>(names.size()) ? names[static_cast<size_t>(i)] : names.front();
   };
   d.setText = [m, names](H& h, const std::string& s) {
      const int i = sim::IndexOf(names, s);
      if (i < 0)
         return false;
      (h.State().*m).store(i);
      return true;
   };
   d.choices = [names](H&) { return names; };
   return d;
}

// An On/Off switch bound to a SceneState atomic<bool>.
PropDef OnOff(const char* device, const std::string& name, Tier tier, const std::string& key,
              std::atomic<bool> SceneState::*m, const char* help)
{
   PropDef d = Row(device, name, tier, key, help);
   d.kind = PropKind::Text;
   d.getText = [m](H& h) { return std::string((h.State().*m).load() ? "On" : "Off"); };
   d.setText = [m](H& h, const std::string& s) {
      if (s != "On" && s != "Off")
         return false;
      (h.State().*m).store(s == "On");
      return true;
   };
   d.choices = [](H&) { return std::vector<std::string>{ "On", "Off" }; };
   return d;
}

// A number that is an engine scope option.
PropDef Opt(const char* device, const std::string& name, Tier tier, const std::string& option, double lo, double hi,
            const char* help)
{
   PropDef d = Row(device, name, tier, option, help);
   d.lo = lo;
   d.hi = hi;
   d.get = [option](H& h) { return h.State().Option(option); };
   d.set = [option](H& h, double v) { h.State().SetOption(option, v); };
   return d;
}

// One of named values that is an engine scope option (value = index + offset).
PropDef OptNames(const char* device, const std::string& name, Tier tier, const std::string& option,
                 const std::vector<std::string>& names, int offset, const char* help)
{
   PropDef d = Row(device, name, tier, option, help);
   d.kind = PropKind::Text;
   d.getText = [option, names, offset](H& h) {
      const int i = static_cast<int>(h.State().Option(option)) - offset;
      return i >= 0 && i < static_cast<int>(names.size()) ? names[static_cast<size_t>(i)] : names.front();
   };
   d.setText = [option, names, offset](H& h, const std::string& s) {
      const int i = sim::IndexOf(names, s);
      if (i < 0)
         return false;
      h.State().SetOption(option, i + offset);
      return true;
   };
   d.choices = [names](H&) { return names; };
   return d;
}

// One of a list of numbers that is an engine scope option (a dropdown; value = the number).
PropDef OptValues(const char* device, const std::string& name, Tier tier, const std::string& option,
                  const std::vector<double>& values, const char* help)
{
   std::vector<std::string> names;
   for (double v : values) names.push_back(FormatNumber(v));
   PropDef d = Row(device, name, tier, option, help);
   d.kind = PropKind::Text;
   d.getText = [option](H& h) { return FormatNumber(h.State().Option(option)); };
   d.setText = [option, names, values](H& h, const std::string& s) {
      const int i = sim::IndexOf(names, s);
      if (i < 0)
         return false;
      h.State().SetOption(option, values[static_cast<size_t>(i)]);
      return true;
   };
   d.choices = [names](H&) { return names; };
   return d;
}

// An On/Off switch that is an engine scope option (1 / 0).
PropDef OnOffOpt(const char* device, const std::string& name, Tier tier, const std::string& option, const char* help)
{
   PropDef d = Row(device, name, tier, option, help);
   d.kind = PropKind::Text;
   d.getText = [option](H& h) { return std::string(h.State().Option(option) != 0 ? "On" : "Off"); };
   d.setText = [option](H& h, const std::string& v) {
      if (v != "On" && v != "Off")
         return false;
      h.State().SetOption(option, v == "On" ? 1 : 0);
      return true;
   };
   d.choices = [](H&) { return std::vector<std::string>{ "On", "Off" }; };
   return d;
}

// Wraps a row's setter: after the value is stored, run a coupling (only when the value changed).
void ThenNumber(PropDef& d, std::function<void(H&)> after)
{
   auto get = d.get;
   auto set = d.set;
   d.set = [get, set, after](H& h, double v) {
      const double old = get(h);
      set(h, v);
      if (get(h) != old)
         after(h);
   };
}

void ThenText(PropDef& d, std::function<void(H&)> after)
{
   auto get = d.getText;
   auto set = d.setText;
   d.setText = [get, set, after](H& h, const std::string& s) {
      const std::string old = get(h);
      if (!set(h, s))
         return false;
      if (get(h) != old)
         after(h);
      return true;
   };
}

// The pixel size follows the sensor pixel, the objective and the emission path.
void NotifyPixel(H& h)
{
   h.Notify("pixel-nm");
}

// ---- the label readouts and the dye fields ----

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
      { "on-sec", "OnSec", { MODE_DSTORM, MODE_PALM } },
      { "off-sec", "OffSec", { MODE_DSTORM, MODE_PALM } },
      { "bleach-prob", "BleachProb", { MODE_DSTORM, MODE_PALM } },
      { "initial-on-sec", "InitialOnSec", { MODE_DSTORM } },
      { "activation-405", "Activation405", { MODE_DSTORM, MODE_PALM } },
      { "spont-activation", "SpontActivation", { MODE_PALM } },
      { "primed", "Primed", { MODE_PALM } },
      { "pre-photon-budget", "PrePhotonBudget", { MODE_PALM } },
      { "photon-cv", "PhotonCv", { MODE_DSTORM, MODE_PALM, MODE_DNA_PAINT } },
      { "photon-budget", "PhotonBudget", { MODE_WIDEFIELD } },
   };
   return rows;
}

// A dye's field value in a mode (the viewer's getters); NaN where the field does not apply.
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
   if (f == "photon-cv") return m.photonCV;
   if (f == "photon-budget") return m.photonBudget;
   return sim::kNaN;
}

// A custom dye slot's field (prefix "dye<N>"): the user's edit, or its source dye's value in its default mode.
PropDef SlotField(int slot, const FieldRow& r)
{
   const std::string prefix = "dye" + std::to_string(slot + 1);
   const std::string key = prefix + "." + r.field;   // the edit's key (the engine option)
   PropDef d = Row("Fluorophores", "Dye" + std::to_string(slot + 1) + "_" + r.suffix, Tier::Expert, prefix + ".fields",
                   "A field of this custom dye (a library value until edited; 0 where it does not apply).");
   auto lib = [slot, r](H& h) {
      std::vector<sim::DyeSlot> slots(3);
      for (int n = 0; n < 3; ++n)
         slots[static_cast<size_t>(n)].source = static_cast<int>(h.State().Option("dye" + std::to_string(n + 1) + ".source"));
      sim::EffectiveDye eff;
      std::string err;
      if (!sim::MakeEffectiveDye(slots[static_cast<size_t>(slot)].source, slots, {}, -1, eff, err))
         return sim::kNaN;
      return FieldValue(eff.dye, eff.mode, r);
   };
   d.get = [key, lib](H& h) {
      double v;
      if (h.State().DyeEdit(key, v))
         return v;
      const double l = lib(h);
      return std::isnan(l) ? 0.0 : l;
   };
   d.set = [key, lib](H& h, double v) {
      const double l = lib(h);
      if (std::isnan(l))
         return;   // not a field of this dye in this mode: nothing to override
      // MM keeps a few digits only: a value that reads back as the library's is the library's.
      if (std::fabs(v - l) <= 1e-6 * std::max(1.0, std::fabs(l)) || FormatNumber(v) == FormatNumber(l))
         h.State().ClearDyeEdit(key);
      else
         h.State().SetDyeEdit(key, v);
   };
   return d;
}

// A target's row-name prefix ("Microtubules", "NuclearPores", "Docking"; the engine's option texts).
std::string TargetRowName(int s) { return sim::ScopeTargetRow(s); }

// A target's name in lower case, for the help texts ("microtubules", "docking strands").
std::string TargetWhat(int s)
{
   std::string n = sim::TargetAt(s).name;
   for (char& c : n)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return n;
}

// Target s's label as the engine sees it: 0 detected %, 1 effective emission nm, 2 detected photons/s ON.
PropDef LabelReadout(int s, const char* suffix, int what, const std::string& help)
{
   PropDef d = Row("Fluorophores", TargetRowName(s) + "_" + suffix, Tier::Advanced,
                   std::string(sim::TargetAt(s).prefix) + "-readout-" + std::to_string(what), "");
   d.help = help;
   d.readOnly = true;
   d.invalidate = Invalidate::None;
   const int structure = sim::TargetAt(s).structure;
   d.get = [structure, what](H& h) {
      double x = 0, y = 0;
      h.Stage().PositionXyAt(sim::SharedStageState::Clock::now(), x, y);
      sim::ScopeStateReadout r;
      std::string err;
      if (sim::ScopeLabelState(BuildScopeSpec(h.State(), x, y, 0, 60, 1), structure, false, r, err) && r.emits)
         return what == 0 ? 100 * r.detectedFraction : what == 1 ? r.lambdaNm : r.detectedPerSec;
      return 0.0;
   };
   d.set = [](H&, double) {};
   return d;
}

// A target's label (structure s of a specimen device): Typical (the target's typical dye in its mode, data/dyes
// library.json typicalLabels), every library dye with data in that mode, in Expert the custom dyes Dye1-3, and None (no
// label: the target is off). The list follows the mode (InSiliScopeHub::TargetModeChanged rebuilds it).
PropDef TargetLabel(const char* device, int s)
{
   const sim::TargetData& t = sim::TargetAt(s);
   const std::string option = std::string(t.prefix) + "-dye";
   PropDef d = Row(device, TargetRowName(s) + "_Label", Tier::Basic, option,
                   "The label: Typical (the usual dye for this target's Mode), any dye with data in that mode (Dye1-3: "
                   "the custom dyes of Fluorophores, Expert), or None (no label: the target is off). A pick applies the "
                   "Lasers Auto preset.");
   d.kind = PropKind::Text;
   d.getText = [option](H& h) {
      const int v = static_cast<int>(h.State().Option(option));
      return v < 0 ? std::string("Typical") : sim::DyeChoices()[static_cast<size_t>(v)];
   };
   d.choices = [s](H& h) { return h.LabelChoices(s); };
   d.setText = [option, s](H& h, const std::string& v) {
      const std::vector<std::string> allowed = h.LabelChoices(s);
      if (std::find(allowed.begin(), allowed.end(), v) == allowed.end())
         return false;
      const double old = h.State().Option(option);
      const double now = v == "Typical" ? -1 : sim::IndexOf(sim::DyeChoices(), v);
      h.State().SetOption(option, now);
      if (now != old)
         h.LoadTargetDye(s, true);
      return true;
   };
   return d;
}

// A target's label rows on its specimen device: its mode and label (Basic), the labelled %, the DNA-PAINT imager
// concentration (NanoMol, not nm) and binding kinetics (the target's, not the dye's) (Advanced), and for a target
// with a linkage model the attachment (Advanced) and its four numbers (Expert); the engine options <prefix>-mode,
// -dye, -label-pct, -imager-nm, -paint-kon, -paint-on-sec, -binder*.
void AddTargetLabelRows(std::vector<PropDef>& t, const char* device, int s)
{
   using namespace sim;
   const std::string P = TargetRowName(s), prefix = TargetAt(s).prefix, what = TargetWhat(s);
   PropDef d = OptNames(device, P + "_Mode", Tier::Basic, prefix + "-mode", DyeModeNames(), 0, "");
   d.help = "How the " + what + "' dye emits (each target has its own): dSTORM, PALM, DNA-PAINT or WideField. A change "
            "sets the labelled % to the mode's suggestion and the label to Typical where the dye has no data for it.";
   ThenText(d, [s](H& h) { h.TargetModeChanged(s); });
   t.push_back(d);
   t.push_back(TargetLabel(device, s));
   d = Opt(device, P + "_LabelingPct", Tier::Advanced, prefix + "-label-pct", 0, 100, "");
   d.help = "Labelled fraction of the " + what + "' sites, % (a mode change sets its suggestion).";
   t.push_back(d);
   t.push_back(Opt(device, P + "_ImagerNanoMol", Tier::Advanced, prefix + "-imager-nm", 0, 10000,
                   "DNA-PAINT imager concentration in the buffer, nanomolar: binding events per site per second = "
                   "k_on x c, so it sets how many sites blink per frame (a change acts at once); the free imager adds a "
                   "uniform background."));
   t.push_back(Opt(device, P + "_BoundTimeSec", Tier::Advanced, prefix + "-paint-on-sec", 0.001, 100,
                   "DNA-PAINT: mean time an imager stays bound to a docking site, s (exponential; half-time = 0.693 x "
                   "this). A property of the docking/imager sequence, so of the target, not of the dye."));
   t.push_back(Opt(device, P + "_KonPerMolarPerSec", Tier::Advanced, prefix + "-paint-kon", 0, 1e9,
                   "DNA-PAINT: imager association rate k_on, per molar per second; binding events per site per second "
                   "= k_on x the imager concentration."));
   if (TargetAt(s).linkageDefault < 0)
      return;
   // The label linkage (data/dyes/library.json linkagePresets, with their references; Legacy = the numbers before it).
   std::vector<std::string> presets;
   for (int i = 0; i < static_cast<int>(LinkagePresetIds().size()); ++i)
      presets.push_back(LinkagePresetAt(i).id);
   d = OptNames(device, P + "_Binder", Tier::Advanced, prefix + "-binder", presets, 0, "");
   d.help = "How the " + what + "' dye is attached: the probe (antibodies, nanobody, SNAP/Halo tag, FP fusion; with "
            "or without a DNA-PAINT docking strand) sets the dye's distance from the structure. Legacy: the built-in "
            "numbers before the linkage model.";
   t.push_back(d);
   const char* rows[4][3] = {
      { "BinderNm", "binder-nm", "The probe's length from the binding site, outward along the structure's axis, nm (-1 = the Binder preset's)." },
      { "BinderFlexNm", "binder-flex-nm", "The probe's flexibility: its end lies in a ball of this radius, nm (-1 = the preset's)." },
      { "LinkerMinNm", "linker-min-nm", "Shortest chemical linker from the probe to the dye, nm (-1 = the preset's)." },
      { "LinkerMaxNm", "linker-max-nm", "Longest linker, nm (uniform in volume between the two; -1 = the preset's)." },
   };
   for (const auto& r : rows)
      t.push_back(Opt(device, P + "_" + r[0], Tier::Expert, prefix + "-" + r[1], -1, 1000, r[2]));
}

// A target's dye-orientation rows (Expert): <prefix>-orient, -orient-polar-deg, -orient-azimuth-deg, -wobble-deg,
// -motion.
void AddTargetOrientRows(std::vector<PropDef>& t, const char* device, int s)
{
   const std::string P = TargetRowName(s), prefix = sim::TargetAt(s).prefix;
   PropDef d = OptNames(device, P + "_Orientation", Tier::Expert, prefix + "-orient", { "Free", "Fixed", "Random" }, 0,
                        "");
   d.help = "Dipole orientation of the " + TargetWhat(s) + "' dyes.";
   t.push_back(d);
   t.push_back(Opt(device, P + "_OrientPolarDeg", Tier::Expert, prefix + "-orient-polar-deg", 0, 180,
                   "Fixed dipole: polar angle."));
   t.push_back(Opt(device, P + "_OrientAzimuthDeg", Tier::Expert, prefix + "-orient-azimuth-deg", -360, 360,
                   "Fixed dipole: azimuth."));
   t.push_back(Opt(device, P + "_WobbleConeDeg", Tier::Expert, prefix + "-wobble-deg", 0, 90,
                   "Dipole wobble cone half-angle."));
   t.push_back(OptNames(device, P + "_Motion", Tier::Expert, prefix + "-motion", { "Static" }, 0, "The dyes' motion."));
}

// A target's readouts on Fluorophores (Advanced): the dye and mode its label resolves to, and the light path's view.
void AddTargetReadoutRows(std::vector<PropDef>& t, int s)
{
   const std::string P = TargetRowName(s), prefix = sim::TargetAt(s).prefix, what = TargetWhat(s);
   PropDef d = Row("Fluorophores", P + "_EffectiveDye", Tier::Advanced, prefix + "-effective", "");
   d.help = "The dye and mode the " + what + "' label resolves to (Typical: the usual dye for the mode; read-only).";
   d.kind = PropKind::Text;
   d.readOnly = true;
   d.invalidate = Invalidate::None;
   d.getText = [s](H& h) {
      sim::EffectiveDye eff;
      int choice = 0;
      std::string err;
      if (!sim::ScopeStructureDye(BuildScopeSpec(h.State(), 0, 0, 0, 0, 1), s, eff, choice, err))
         return err;
      return std::string(eff.dye.id) + " (" + sim::DyeModeNames()[static_cast<size_t>(eff.mode)] + ")";
   };
   d.setText = [](H&, const std::string&) { return true; };
   t.push_back(d);
   t.push_back(LabelReadout(s, "DetectedPct", 0,
                            "The " + what + "' dye: % of its emission the light path detects (read-only)."));
   t.push_back(LabelReadout(s, "EmissionNm", 1,
                            "The " + what + "' dye: effective detected wavelength, nm (the PSF's; read-only)."));
   t.push_back(LabelReadout(s, "PhotonsPerSecOn", 2,
                            "The " + what + "' dye: detected photons per second while ON (read-only)."));
}

// ---- the objective turret's catalogue ----

std::vector<PropDef> BuildTable()
{
   using namespace sim;
   std::vector<PropDef> t;
   auto add = [&t](PropDef d) { t.push_back(std::move(d)); };

   // =========================== Camera ===========================
   {
      const char* C = "Camera";
      PropDef d = Row(C, "CameraPreset", Tier::Basic, "camera-preset",
                      "A camera model: sets the sensor type, QE curve, noise values, gain and sensor pixel.");
      d.kind = PropKind::Text;
      d.getText = [](H& h) {
         const int i = h.State().cameraPreset.load();
         return i >= 0 && i < static_cast<int>(CameraIds().size()) ? CameraIds()[static_cast<size_t>(i)] : CameraIds().front();
      };
      d.setText = [](H& h, const std::string& s) {
         const int i = IndexOf(CameraIds(), s);
         if (i < 0)
            return false;
         h.ApplyCameraPreset(i);
         return true;
      };
      d.choices = [](H&) { return CameraIds(); };
      add(d);

      d = Row(C, "PixelSizeNm", Tier::Advanced, "pixel-nm",
              "The pixel size in the sample: SensorPixelUm / (objective x emission magnification).");
      d.readOnly = true;
      d.invalidate = Invalidate::None;
      d.get = [](H& h) { return h.State().PixelSizeNm(); };
      d.set = [](H&, double) {};
      add(d);
      d = Num(C, "SensorPixelUm", Tier::Advanced, "sensor-pixel-um", &SceneState::sensorPixelUm, 0.5, 50,
              "The sensor's physical pixel pitch (um); the camera preset sets it.");
      ThenNumber(d, NotifyPixel);
      add(d);
      add(Names(C, "CameraType", Tier::Advanced, "camera-type", &SceneState::emccd, { "sCMOS", "EMCCD" },
                "The noise chain: sCMOS (per-pixel maps) or EMCCD (gain register, CIC); the camera preset sets it."));
      add(Num(C, "QuantumEfficiency", Tier::Advanced, "qe", &SceneState::quantumEfficiency, 0.01, 1,
              "The QE where the QE curve is Flat (and for BrightField without a curve)."));
      add(OptNames(C, "QeCurve", Tier::Advanced, "qe-curve", CameraIds(), 0,
                   "The QE curve the detected fractions use (a camera's FPbase curve, or Custom = flat)."));
      d = Num(C, "GainElectronsPerADU", Tier::Advanced, "gain", &SceneState::gainElectronsPerAdu, 0.0001, 100,
              "Conversion gain, electrons per ADU, per photoelectron (an EMCCD's includes its EM gain).");
      ThenNumber(d, [](H& h) { h.Notify("em-gain"); });
      add(d);
      add(Num(C, "OffsetADU", Tier::Advanced, "offset", &SceneState::offsetAdu, 0, 10000, "The camera offset (ADU)."));
      add(Num(C, "OffsetStdADU", Tier::Advanced, "offset-std", &SceneState::offsetStdAdu, 0, 500,
              "The pixel-to-pixel spread of the offset (ADU, a static pattern)."));
      add(Num(C, "ReadNoiseElectrons", Tier::Advanced, "read-noise", &SceneState::readNoiseElectrons, 0, 100,
              "Read noise, electrons RMS (an EMCCD's before the gain register)."));
      add(Num(C, "DarkCurrentElectronsPerSec", Tier::Advanced, "dark-per-sec", &SceneState::darkCurrentPerSec, 0, 100,
              "Dark current, electrons per pixel per second."));
      add(Num(C, "sCMOS_GainStdPctPerPixel", Tier::Expert, "gain-std-pct", &SceneState::pixelGainStdPct, 0, 50,
              "sCMOS only: pixel-to-pixel spread of the gain, % (a static pattern)."));
      add(Num(C, "sCMOS_ReadNoiseStdPctPerPixel", Tier::Expert, "read-noise-std-pct", &SceneState::pixelReadNoiseStdPct,
              0, 100, "sCMOS only: pixel-to-pixel spread of the read noise, %."));
      add(Num(C, "EMCCD_CicElectrons", Tier::Expert, "cic", &SceneState::cicElectrons, 0, 1,
              "EMCCD only: clock-induced charge, electrons per pixel per frame."));
      d = Row(C, "EMCCD_EmGain", Tier::Expert, "em-gain",
              "EMCCD only: the EM gain, the preset's pre-amplifier sensitivity / the gain (read-only).");
      d.readOnly = true;
      d.invalidate = Invalidate::None;
      d.get = [](H& h) { return h.State().EmGain(); };
      d.set = [](H&, double) {};
      add(d);
      add(Int(C, "BitDepth", Tier::Expert, "bit-depth", &SceneState::bitDepth, 8, 16,
              "The ADC's bit depth (the EMCCD path clips to it)."));
   }

   // =========================== XYStage ===========================
   {
      const char* X = "XYStage";
      PropDef d = Row(X, "StageSpeedUmPerSec", Tier::Advanced, "xy-speed", "Speed of a stage move, um/s.");
      d.lo = 1;
      d.hi = 100000;
      d.invalidate = Invalidate::None;
      d.get = [](H& h) { return h.Stage().XySpeed(); };
      d.set = [](H& h, double v) { h.Stage().SetXySpeed(v); };
      add(d);
      d = Row(X, "StageSettleMs", Tier::Advanced, "xy-settle", "Time the stage stays busy after arriving, ms.");
      d.lo = 0;
      d.hi = 10000;
      d.invalidate = Invalidate::None;
      d.get = [](H& h) { return h.Stage().XySettleSec() * 1000.0; };
      d.set = [](H& h, double v) { h.Stage().SetXySettleSec(v / 1000.0); };
      add(d);
      d = Row(X, "StageLimitUm", Tier::Advanced, "xy-limit", "Travel limit, +/- um around the origin.");
      d.invalidate = Invalidate::None;
      d.get = [](H& h) { return h.State().xyLimitUm.load(); };
      d.set = [](H& h, double v) { h.State().xyLimitUm = std::max(1.0, std::min(v, 1.0e7)); };
      add(d);
   }

   // =========================== Objective ===========================
   {
      const char* O = "Objective";
      PropDef d = Num(O, "NA", Tier::Advanced, "na", &SceneState::na, 0.1, 1.7,
                      "Numerical aperture (setting it by hand makes the turret position Custom).");
      ThenNumber(d, [](H& h) { h.Notify("objective"); });
      add(d);
      d = Num(O, "ImmersionIndex", Tier::Advanced, "immersion-index", &SceneState::immersionIndex, 1.0, 2.0,
              "Refractive index of the immersion medium.");
      ThenNumber(d, [](H& h) { h.Notify("objective"); });
      add(d);
      d = Num(O, "Magnification", Tier::Advanced, "objective-mag", &SceneState::objectiveMag, 1, 250,
              "The objective's magnification (the pixel size follows).");
      ThenNumber(d, [](H& h) {
         h.Notify("objective");
         NotifyPixel(h);
      });
      add(d);
      d = Row(O, "ZernikePreset", Tier::Basic, "zernike-preset",
              "The objective's aberrations: a named Zernike set (sets ZernikeCoefficients).");
      d.kind = PropKind::Text;
      d.getText = [](H& h) { return h.State().ZernikePreset(); };
      d.setText = [](H& h, const std::string& s) {
         if (IndexOf(ZernikePresetNames(), s) < 0)
            return false;
         h.ApplyZernikePreset(s);
         return true;
      };
      d.choices = [](H&) { return ZernikePresetNames(); };
      add(d);
      add(Num(O, "PsfKernelHalfWidthNm", Tier::Advanced, "psf-kernel-half-width-nm", &SceneState::psfKernelHalfWidthNm,
              100, 20000, "Half-width of the rendered PSF window (nm; a minimum, grown to cover the first Airy ring)."));
      add(Num(O, "PsfZRangeUm", Tier::Advanced, "psf-z-range-um", &SceneState::psfZRangeUm, 0.1, 20,
              "The z span of the PSF kernel (um): dyes further from focus use its end plane."));
      add(Num(O, "PsfZStepUm", Tier::Advanced, "psf-z-step-um", &SceneState::psfZStepUm, 0.01, 1,
              "The z step of the PSF kernel's planes (um)."));
      d = Row(O, "ZernikeCoefficients", Tier::Expert, "zernike",
              "The pupil's Zernike coefficients, OSA 0-27, in waves, space-separated (GibsonLanniZernike).");
      d.kind = PropKind::Text;
      d.getText = [](H& h) { return h.State().ZernikeCoefficients(); };
      d.setText = [](H& h, const std::string& s) {
         bool ok = false;
         ParseZernikeCoefficients(s, ok);
         if (!ok)
            return false;
         h.State().SetZernikeCoefficients(s);
         return true;
      };
      add(d);
      add(Num(O, "WorkingDistanceUm", Tier::Expert, "working-distance-um", &SceneState::workingDistanceUm, 0, 9999,
              "Working distance (um; the JVM GibsonLanni model only)."));
   }

   // =========================== EmissionPath ===========================
   {
      PropDef d = Num("EmissionPath", "EmissionMagnification", Tier::Basic, "emission-mag", &SceneState::emissionMag,
                      0.1, 10, "Magnification between the objective and the camera (tube lens / relay; goes with the camera).");
      ThenNumber(d, [](H& h) {
         h.Notify("emission-mag");   // the magnifier tells MM
         NotifyPixel(h);
      });
      add(d);
   }

   // =========================== ExcitationFilter, Dichroic, EmissionFilter (custom edges) ===========================
   add(Opt("ExcitationFilter", "CustomLoNm", Tier::Expert, "ex-lo-nm", 300, 900,
           "The Custom excitation filter's lower edge (nm)."));
   add(Opt("ExcitationFilter", "CustomHiNm", Tier::Expert, "ex-hi-nm", 300, 900,
           "The Custom excitation filter's upper edge (nm)."));
   add(Opt("Dichroic", "CustomEdgeNm", Tier::Expert, "dichroic-edge-nm", 300, 900,
           "The Custom dichroic's edge (nm; a long pass)."));
   add(Opt("EmissionFilter", "CustomLoNm", Tier::Expert, "em-lo-nm", 300, 900,
           "The Custom emission filter's lower edge (nm)."));
   add(Opt("EmissionFilter", "CustomHiNm", Tier::Expert, "em-hi-nm", 300, 900,
           "The Custom emission filter's upper edge (nm)."));

   // =========================== Lasers ===========================
   {
      const char* L = "Lasers";
      for (int nm : LaserLines())
      {
         PropDef d = Opt(L, "Laser" + std::to_string(nm) + "KWcm2", Tier::Basic, "laser-" + std::to_string(nm), 0, 10,
                         "Intensity of this laser line at the sample, kW/cm^2 (0 = off).");
         ThenNumber(d, [](H& h) { h.ClearLightPreset(); });
         add(d);
      }
      PropDef d = Row(L, "Preset", Tier::Basic, "light-preset",
                      "A light path (lasers + filter cube). Auto: the labelled targets' dyes' presets together (each laser "
                      "line at its highest power; the multi-band filter set where their filters differ), what a dye or "
                      "mode pick applies. A preset by name: one dye mode's. None = as set.");
      d.kind = PropKind::Text;
      d.getText = [](H& h) { return h.State().LightPreset(); };
      d.setText = [](H& h, const std::string& s) {
         if (s == "None")
            h.State().SetLightPreset(s);
         else if (s != "Auto" && !FindLightPreset(s))
            return false;
         else
            h.ApplyLightPreset(s);
         return true;
      };
      d.choices = [](H&) {
         std::vector<std::string> v = { "None", "Auto" };
         v.insert(v.end(), LightPresetIds().begin(), LightPresetIds().end());
         return v;
      };
      add(d);
      add(OptNames(L, "IlluminationProfile", Tier::Basic, "illum-profile", sim::IlluminationProfileNames(), 0,
                   "The excitation beam's cross-section: Flat, Gaussian or FlatTop (a disc with a soft edge)."));
      add(Opt(L, "IlluminationFwhmPct", Tier::Basic, "illum-fwhm", 10, 300,
              "The beam's FWHM (Gaussian, FlatTop), % of the field of view's width."));
      // The illumination geometry: the laser spot's place in the objective's back focal plane (spec/ALGORITHM.md
      // "Excitation geometry"). The config group Illumination sets it to Epi / HILO / TIRF.
      add(Opt(L, "BfpPosition", Tier::Basic, "illum-bfp", 0, 1.2,
              "Where the laser spot sits in the objective's back focal plane: 0 = its centre (epi), 1 = the critical "
              "angle; between: HILO (an inclined sheet), above: TIRF (an evanescent layer), up to the pupil edge "
              "NA / medium index (beyond it no light reaches the sample)."));
      {
         // The geometry's readouts (the engine's excitation for the current settings).
         auto excitation = [](H& h, sim::Excitation& ex) {
            std::vector<sim::ExcitationLine> shares;
            std::string err;
            return sim::ScopeExcitation(BuildScopeSpec(h.State(), 0, 0, 0, 0, 1), ex, shares, err);
         };
         PropDef r = Row(L, "IlluminationRegime", Tier::Advanced, "illum-regime",
                         "Epi, HILO, TIRF, or Clipped (the spot beyond the pupil edge: no light) (read-only).");
         r.kind = PropKind::Text;
         r.readOnly = true;
         r.invalidate = Invalidate::None;
         r.getText = [excitation](H& h) {
            sim::Excitation ex;
            return excitation(h, ex) ? sim::IlluminationRegimeNames()[static_cast<size_t>(ex.Regime())] : std::string("Epi");
         };
         r.setText = [](H&, const std::string&) { return true; };
         add(r);
         r = Row(L, "IlluminationDepthNm", Tier::Advanced, "illum-depth-nm",
                 "TIRF: the evanescent field's 1/e intensity depth at the first label's main line; HILO: the sheet's "
                 "thickness on the axis; -1 in epi (nm, read-only).");
         r.readOnly = true;
         r.invalidate = Invalidate::None;
         r.get = [excitation](H& h) {
            sim::Excitation ex;
            std::vector<sim::ExcitationLine> shares;
            std::string err;
            if (!sim::ScopeExcitation(BuildScopeSpec(h.State(), 0, 0, 0, 0, 1), ex, shares, err))
               return -1.0;
            return ex.DepthNm(shares.empty() ? 640.0 : shares[0].nm);
         };
         r.set = [](H&, double) {};
         add(r);
      }
      d = Opt(L, "CustomLineNm", Tier::Advanced, "laser-custom-nm", 0, 1000, "Wavelength of an extra laser line (nm).");
      ThenNumber(d, [](H& h) { h.ClearLightPreset(); });
      add(d);
      d = Opt(L, "CustomLineKWcm2", Tier::Advanced, "laser-custom", 0, 10, "Intensity of the extra line, kW/cm^2.");
      ThenNumber(d, [](H& h) { h.ClearLightPreset(); });
      add(d);
      add(Opt(L, "ChamberHeightUm", Tier::Advanced, "chamber-height-um", 0, 1000,
              "Depth of the imager solution over the coverslip (um): the DNA-PAINT free-imager background is its "
              "column of light (epi: all of it; HILO: about the sheet; TIRF: about the penetration depth)."));
   }

   // =========================== TransmittedLamp ===========================
   {
      const char* T = "TransmittedLamp";
      add(Arr(T, "IntensityPhotonsPerPxPerSec", Tier::Basic, "bf-photons", &SceneState::brightField,
              BF_PHOTONS_PER_PX_PER_SEC, 0, 1e9, "Photons per pixel per second through an empty field."));
      add(Arr(T, "CondenserNA", Tier::Advanced, "bf-condenser-na", &SceneState::brightField, BF_CONDENSER_NA, 0, 1.5,
              "Condenser numerical aperture (the partial coherence of the illumination)."));
      add(Arr(T, "WavelengthNm", Tier::Advanced, "bf-wavelength-nm", &SceneState::brightField, BF_WAVELENGTH_NM, 300,
              1000, "The lamp's (filtered) wavelength, nm."));
      add(Arr(T, "UseObjectiveAberrations", Tier::Expert, "bf-aberrations", &SceneState::brightField, BF_ABERRATIONS, 0,
              1, "1: the detection pupil carries the objective's Zernike aberrations.", true));
   }

   // =========================== SampleHolder ===========================
   {
      const char* S = "SampleHolder";
      // The medium over the coverslip, for every specimen: BrightField's index and the critical angle of TIRF.
      add(Arr(S, "MediumIndex", Tier::Advanced, "bf.IndexMedium", &SceneState::brightField, BF_INDEX_MEDIUM, 1.0, 2.0,
              "Refractive index of the medium over the coverslip (BrightField; the critical angle of Lasers.BfpPosition)."));
      {
         PropDef d = Row(S, "DriftPreset", Tier::Basic, "drift-preset",
                         "How much the sample drifts: sets the xy and z drift speeds (0, 2, 5, 25, 250 nm/s) and random "
                         "walks (0, 0.4, 1, 5, 50 nm/sqrt s). Custom = as set.");
         d.kind = PropKind::Text;
         d.getText = [](H& h) {
            const int i = h.State().driftPreset.load();
            return i >= 0 && i < static_cast<int>(DriftPresetNames().size()) ? DriftPresetNames()[static_cast<size_t>(i)]
                                                                              : DriftPresetNames().back();
         };
         d.setText = [](H& h, const std::string& s) {
            const int i = IndexOf(DriftPresetNames(), s);
            if (i < 0)
               return false;
            h.ApplyDriftPreset(i);
            return true;
         };
         d.choices = [](H&) { return DriftPresetNames(); };
         add(d);
      }
      {
         static const std::vector<std::string> modes = { "Running", "Paused" };
         PropDef d = Row(S, "TimeWhileIdle", Tier::Basic, "time-while-idle",
                         "Between acquisitions: Running = the sample drifts on and an opened lasers' shutter keeps "
                         "lighting it (bleaching, activating); Paused = nothing changes until the next frame is "
                         "acquired.");
         d.kind = PropKind::Text;
         d.getText = [](H& h) { return modes[h.State().timeWhileIdle.load() == 1 ? 1 : 0]; };
         d.setText = [](H& h, const std::string& s) {
            const int i = IndexOf(modes, s);
            if (i < 0)
               return false;
            h.State().timeWhileIdle = i;
            return true;
         };
         d.choices = [](H&) { return modes; };
         d.invalidate = Invalidate::None;   // read by the producer per frame; no image changes
         add(d);
      }
      auto custom = [](H& h) { h.ClearDriftPreset(); };
      PropDef dd = Arr(S, "DriftXySpeedNmPerSec", Tier::Advanced, "drift-xy-speed", &SceneState::directedDrift,
                       DD_XY_SPEED, 0, 1000, "Directed sample drift in xy, mean speed (nm/s).");
      ThenNumber(dd, custom);
      add(dd);
      dd = Arr(S, "DriftZSpeedNmPerSec", Tier::Advanced, "drift-z-speed", &SceneState::directedDrift, DD_Z_SPEED, 0,
               1000, "Directed sample drift in z, mean speed (nm/s); its direction: DriftZDirection.");
      ThenNumber(dd, custom);
      add(dd);
      dd = Num(S, "DriftXyNmPerSqrtSec", Tier::Advanced, "drift-xy-walk", &SceneState::driftXyNmPerSqrtSec, 0, 200,
               "Random-walk drift in x and y: RMS displacement after 1 s (nm), per axis.");
      ThenNumber(dd, custom);
      add(dd);
      dd = Num(S, "DriftZNmPerSqrtSec", Tier::Advanced, "drift-z-walk", &SceneState::driftZNmPerSqrtSec, 0, 200,
               "Random-walk drift in z: RMS displacement after 1 s (nm).");
      ThenNumber(dd, custom);
      add(dd);
      add(Num(S, "BackgroundPhotonsPerSec", Tier::Advanced, "background-per-sec", &SceneState::backgroundPhotonsPerSec,
              0, 200000, "Uniform background (autofluorescence, out-of-focus light), photons per pixel per second."));
      add(Num(S, "BackgroundDecaySec", Tier::Advanced, "bg-decay-sec", &SceneState::bgDecaySec, 0, 100000,
              "The background fades to 30% with this time constant (s; 0 = no fade)."));
      add(Arr(S, "DriftXyAngleDeg", Tier::Expert, "drift-xy-angle", &SceneState::directedDrift, DD_XY_ANGLE, -1, 360,
              "Direction of the directed xy drift (deg; -1 = random per seed)."));
      {
         static const std::vector<std::string> dirs = { "Random", "Up", "Down" };
         PropDef d = Row(S, "DriftZDirection", Tier::Expert, "drift-z-direction",
                         "Direction the z drift starts in: Up = away from the coverslip, Down = towards it, Random = per "
                         "seed.");
         d.kind = PropKind::Text;
         d.getText = [](H& h) {
            const double v = h.State().directedDrift[DD_Z_DIRECTION].load();
            return dirs[v > 0 ? 1 : v < 0 ? 2 : 0];
         };
         d.setText = [](H& h, const std::string& s) {
            const int i = IndexOf(dirs, s);
            if (i < 0)
               return false;
            h.State().directedDrift[DD_Z_DIRECTION] = i == 1 ? 1.0 : i == 2 ? -1.0 : 0.0;
            return true;
         };
         d.choices = [](H&) { return dirs; };
         add(d);
      }
      add(Arr(S, "DriftXyAngleWanderDeg", Tier::Expert, "drift-angle-wander", &SceneState::directedDrift,
              DD_ANGLE_WANDER, 0, 180, "The xy drift direction swings slowly within +/- this (deg; 180 = any direction)."));
      add(Arr(S, "DriftZAngleWanderDeg", Tier::Expert, "drift-z-angle-wander", &SceneState::directedDrift,
              DD_Z_ANGLE_WANDER, 0, 180,
              "The z drift swings within +/- this (deg): speed x cos(angle); 90 = between full speed and still, 180 = "
              "also back."));
      add(Arr(S, "DriftSpeedWanderPct", Tier::Expert, "drift-speed-wander", &SceneState::directedDrift, DD_SPEED_WANDER,
              0, 100, "RMS wander of the drift speeds, % of the mean."));
      add(Arr(S, "DriftWanderTimeSec", Tier::Expert, "drift-wander-time", &SceneState::directedDrift, DD_WANDER_TIME,
              0.1, 100000, "Correlation time of the wanders (s)."));
      add(Num(S, "PsfSampleIndex", Tier::Expert, "psf-sample-index", &SceneState::psfSampleIndex, 1.0, 2.0,
              "Refractive index of the sample medium in the Gibson-Lanni PSF (= immersion: no mismatch)."));
      add(Num(S, "PsfSampleDepthNm", Tier::Expert, "psf-sample-depth-nm", &SceneState::psfSampleDepthNm, -100000,
              100000, "Depth of the emitters below the coverslip in the Gibson-Lanni PSF (nm)."));
   }

   // =========================== CellField ===========================
   {
      const char* F = "CellField";
      for (int s = 0; s < TargetCount(); ++s)
         if (!std::strcmp(TargetAt(s).specimen, F))
            AddTargetLabelRows(t, F, s);
      const char* help[CF_COUNT] = {
         "Size of a packing block, um.",
         "Fraction of the coverslip covered by cells.",
         "Smallest cell diameter, um.",
         "Largest cell diameter, um.",
         "Microtubule density, per um^2 of cell.",
         "Extra focus offset above the coverslip, um.",
         "Total z slab around the focal plane whose dyes are rendered, um (0 = all).",
         "Nucleus base height, minimum (um).",
         "Nucleus base height, maximum (um).",
         "Nucleus outline irregularity, minimum.",
         "Nucleus outline irregularity, maximum.",
         "Nucleus kidney bend, minimum.",
         "Nucleus kidney bend, maximum.",
         "Nucleus outline smoothness (spectral exponent).",
         "Nucleus thickness irregularity.",
         "Nucleus base widening.",
         "Height of the nucleus' widest point, minimum (fraction).",
         "Height of the nucleus' widest point, maximum (fraction).",
         "Microtubule starts: decay length, % of the cell radius.",
         "Microtubule ends: decay length, % of the cell radius.",
         "Microtubule direction concentration (von Mises kappa).",
         "Nuclear pore complexes per um^2 of nuclear envelope, over the whole envelope (8; measured 5-8.6 in HeLa and "
         "U2OS nuclei; 0 = no pores, the target is off).",
         "Smallest centre distance of two pores, nm (180; a pore is about 120 across).",
         "Radius of a pore's Nup96 rings, nm.",
         "Distance between a pore's cytoplasmic and nucleoplasmic ring, nm.",
         "Diameter of the arc of 4 Nup96 at each of a ring's 8 corners, nm.",
      };
      const char* names[CF_COUNT] = { "ChunkSizeUm", "Occupancy", "CellDiameterMinUm", "CellDiameterMaxUm",
         "MicrotubuleDensityPerUm2", "FocusHeightUm", "ZRangeUm", "NucBaseMinUm", "NucBaseMaxUm", "NucIrregMin",
         "NucIrregMax", "NucBendMin", "NucBendMax", "NucSmooth", "NucThickIrreg", "NucAsym", "NucWidestMin",
         "NucWidestMax", "MicrotubuleStartDecayPct", "MicrotubuleEndDecayPct", "MicrotubuleDirKappa",
         "NuclearPoreDensityPerUm2", "NuclearPoreMinDistNm", "NuclearPoreRadiusNm", "NuclearPoreRingSepNm",
         "NuclearPoreCornerSpreadNm" };
      const double lo[CF_COUNT] = { 4.0, 0.05, 5.0, 5.0, 0.0, -10.0, 0.0,
         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -0.9, 0.0, 0.0, 0.05, 0.5, 0.0, 0.0, 0.0, 20.0, 0.0, 0.0 };
      const double hi[CF_COUNT] = { 200.0, 1.0, 100.0, 100.0, 2.0, 10.0, 50.0,
         2.0, 2.0, 0.3, 0.3, 1.0, 1.0, 4.0, 0.4, 0.9, 1.0, 1.0, 10.0, 50.0, 10.0, 30.0, 500.0, 150.0, 150.0, 30.0 };
      // Cell size and densities are everyday physics; packing blocks, slab and shape parameters are expert.
      for (int i : { CF_CELL_DIAM_MIN_UM, CF_CELL_DIAM_MAX_UM, CF_OCCUPANCY, CF_MT_DENSITY, CF_NPC_DENSITY })
         add(Arr(F, names[i], Tier::Advanced, std::string("cf.") + names[i], &SceneState::cellField, i, lo[i], hi[i],
                 help[i]));
      const char* indexNames[] = { "IndexMedium", "IndexCytoplasm", "IndexNucleus", "IndexMicrotubule" };
      const char* indexHelp[] = { "Refractive index of the medium (BrightField).",
                                  "Refractive index of the cytoplasm (BrightField).",
                                  "Refractive index of the nucleus (BrightField).",
                                  "Refractive index of a microtubule (BrightField)." };
      for (int k = 1; k < 4; ++k)   // (the medium's: SampleHolder.MediumIndex)
         add(Arr(F, indexNames[k], Tier::Advanced, std::string("bf.") + indexNames[k], &SceneState::brightField,
                 BF_INDEX_MEDIUM + k, 1.0, 2.0, indexHelp[k]));
      for (int i = 0; i < CF_COUNT; ++i)
         if (i != CF_CELL_DIAM_MIN_UM && i != CF_CELL_DIAM_MAX_UM && i != CF_OCCUPANCY && i != CF_MT_DENSITY &&
             i != CF_NPC_DENSITY)
            add(Arr(F, names[i], Tier::Expert, std::string("cf.") + names[i], &SceneState::cellField, i, lo[i], hi[i],
                    help[i]));
      add(OnOff(F, "Packing", Tier::Expert, "packing", &SceneState::cellFieldPacking,
                "Relax the cells into a packed field (Off: their seeded positions)."));
      add(Arr(F, "AbsorptionPerUm", Tier::Expert, "bf-absorption", &SceneState::brightField, BF_ABSORPTION_PER_UM, 0,
              100, "Absorption of the cell material, per um (0 = unstained)."));
      for (int s = 0; s < TargetCount(); ++s)
         if (!std::strcmp(TargetAt(s).specimen, F))
            AddTargetOrientRows(t, F, s);
   }

   // =========================== Nanorulers ===========================
   // The DNA-origami nanorulers (specimen 1 of data/specimens.json; the engine's origami-* options = the core params
   // origami*): their docking strands' label, then the molecule and the field.
   {
      const char* N = "Nanorulers";
      for (int s = 0; s < TargetCount(); ++s)
         if (!std::strcmp(TargetAt(s).specimen, N))
            AddTargetLabelRows(t, N, s);
      add(OptNames(N, "Design", Tier::Basic, "origami-design", ScopeOrigamiDesignIds(), 0,
                   "The DNA-origami design (data/origami/designs.json): Nanoruler = a 13 nm rod with three spots of "
                   "docking strands SpacingNm apart and 20 nm ends; TUDelftLogo = a 2D origami, a rigid plate with one "
                   "strand on each of 37 hexagons of the TU Delft logo, neighbours SpacingNm apart."));
      add(OptValues(N, "SpacingNm", Tier::Basic, "origami-spacing-nm", { 640, 320, 160, 80, 40, 20, 10, 5 },
                    "Distance between neighbouring spots of docking strands, nm; the origami grows or shrinks with it "
                    "(the nanoruler keeps its 20 nm ends: 80 nm = a 200 nm rod)."));
      add(Opt(N, "DensityPerUm2", Tier::Basic, "origami-density", 0, 50,
              "Molecules per um^2 of coverslip, before the minimum spacing thins them."));
      PropDef d = Opt(N, "StrandsPerSpot", Tier::Basic, "origami-strands-per-spot", 1, 5,
                      "Docking strands per spot: ring positions 1..N of the nanoruler's 5 carry one (a 2D origami has 1).");
      d.kind = PropKind::Integer;
      add(d);
      add(OnOffOpt(N, "Bending", Tier::Advanced, "origami-bend",
                   "On: the rod bends in the coverslip plane as a worm-like chain (PersistenceLengthNm); Off: rigid."));
      add(Opt(N, "PersistenceLengthNm", Tier::Advanced, "origami-lp-nm", 10, 100000,
              "Bending persistence length, nm (an estimate: measured helix bundles 740 nm - 8.2 um)."));
      add(Opt(N, "MinSpacingNm", Tier::Advanced, "origami-min-spacing-nm", 0, 5000,
              "No two molecule centres closer than this, nm (0 = no thinning)."));
      add(Opt(N, "AttachHeightNm", Tier::Advanced, "origami-attach-nm", 0, 1000,
              "Height of the rod's bottom above the glass (the surface coating), nm."));
      add(Opt(N, "LinkerNm", Tier::Advanced, "origami-linker-nm", 0, 50,
              "A dye lies within this distance of its strand's anchor, outward, nm."));
      add(OptNames(N, "Roll", Tier::Advanced, "origami-roll", { "Uniform", "Fixed" }, 0,
                   "How each rod landed about its own axis: Uniform = a random roll each, Fixed = RollDeg."));
      add(Opt(N, "RollDeg", Tier::Advanced, "origami-roll-deg", -360, 360, "The fixed roll (Roll = Fixed), deg."));
      add(OnOffOpt(N, "KeepHeights", Tier::Expert, "origami-heights",
                   "Off (2D): every strand at the rod axis height; On: the ring's own heights (3D, not validated "
                   "yet)."));
      for (int s = 0; s < TargetCount(); ++s)
         if (!std::strcmp(TargetAt(s).specimen, N))
            AddTargetOrientRows(t, N, s);
   }

   // =========================== Fluorophores ===========================
   {
      // The targets' readouts (each target's mode is its specimen device's <Row>_Mode since issue 15: no experiment-wide
      // mode).
      for (int s = 0; s < TargetCount(); ++s)
         AddTargetReadoutRows(t, s);
      for (int slot = 0; slot < 3; ++slot)
      {
         const std::string option = "dye" + std::to_string(slot + 1) + ".source";
         PropDef src = OptNames("Fluorophores", "Dye" + std::to_string(slot + 1) + "_Source", Tier::Expert, option,
                                DyeIds(), 0, "The library dye this custom dye starts from.");
         ThenText(src, [slot](H& h) { h.DyeSlotSourceChanged(slot); });
         add(src);
         for (const FieldRow& r : FieldRows())
            add(SlotField(slot, r));
      }
   }

   // =========================== Renderer ===========================
   {
      const char* R = "Renderer";
      PropDef d = Row(R, "Quality", Tier::Basic, "quality",
                      "Speed vs fidelity: Fast, Realistic (the reference settings) or Exhaustive. Custom: set by hand.");
      d.kind = PropKind::Text;
      // (BrightField quality level, PSF oversampling, mean-field upscaling, blink halo cut) per quality:
      // Simulation/RenderPresets.h (the cli's --presets-json prints the same table).
      using Q = sim::QualityPreset;
      static const auto& qs = sim::kQualityPresets;
      d.getText = [](H& h) {
         for (const Q& q : qs)
            if (h.State().brightField[BF_QUALITY].load() == q.bf && h.State().psfOversampling.load() == q.os &&
                h.State().wideField[WF_UPSCALING].load() == q.wf && h.State().psfHaloCut.load() == q.halo)
               return std::string(q.name);
         return std::string("Custom");
      };
      d.setText = [](H& h, const std::string& s) {
         if (s == "Custom")
            return true;
         for (const Q& q : qs)
            if (s == q.name)
            {
               h.State().brightField[BF_QUALITY] = q.bf;
               h.State().psfOversampling = q.os;
               h.State().wideField[WF_UPSCALING] = q.wf;
               h.State().psfHaloCut = q.halo;
               h.Notify("bf-quality");
               h.Notify("psf-oversampling");
               h.Notify("wf-upscale");
               h.Notify("psf-halo-cut");
               return true;
            }
         return false;
      };
      d.choices = [](H&) { return std::vector<std::string>{ "Fast", "Realistic", "Exhaustive", "Custom" }; };
      add(d);
      auto quality = [](H& h) { h.Notify("quality"); };

      d = OnOff(R, "UseGpu", Tier::Advanced, "use-gpu", &SceneState::useGpu,
                "Render on the GPU (Direct3D 11) where a path exists; GpuStatus says which is used.");
      add(d);
      d = Row(R, "GpuStatus", Tier::Advanced, "gpu-status", "What the camera renders on, or why the CPU (read-only).");
      d.kind = PropKind::Text;
      d.readOnly = true;
      d.invalidate = Invalidate::None;
      d.getText = [](H& h) { return h.State().GpuStatus(); };
      d.setText = [](H&, const std::string&) { return true; };
      add(d);
      // The same for the checks at any Detail (tools/bench_live.py records it).
      d = Row(R, "Test_GpuStatus", Tier::Test, "test-gpu-status", "GpuStatus at any Detail (read-only).");
      d.kind = PropKind::Text;
      d.readOnly = true;
      d.invalidate = Invalidate::None;
      d.getText = [](H& h) { return h.State().GpuStatus(); };
      d.setText = [](H&, const std::string&) { return true; };
      add(d);
      d = Names(R, "DiskCache", Tier::Advanced, "disk-cache", &SceneState::diskCacheMode, { "Off", "Cells", "CellsAndPsf" },
                "Keep packed cells (and the PSF kernel, ~200 MB) in the per-user cache directory between sessions.");
      d.invalidate = Invalidate::Live;   // a cache only: no output changes
      {
         auto set = d.setText;
         d.setText = [set](H& h, const std::string& s) {
            if (!set(h, s))
               return false;
            sim::SetPsfKernelDiskCacheDir(h.State().diskCacheMode.load() >= 2 ? sim::DefaultCacheDir() : std::string());
            return true;
         };
      }
      add(d);
      add(Names(R, "PsfModel", Tier::Expert, "psf-model", &SceneState::psfModel,
                { "Gaussian", "RichardsWolf", "GibsonLanni", "GibsonLanniZernike" },
                "The PSF model (GibsonLanniZernike: scalar Gibson-Lanni + Zernike pupil; RichardsWolf/GibsonLanni: JVM)."));
      d = Int(R, "PsfOversampling", Tier::Expert, "psf-oversampling", &SceneState::psfOversampling, 1, 16,
              "PSF kernel samples per camera pixel.");
      ThenNumber(d, quality);
      add(d);
      add(Names(R, "PsfInterp", Tier::Expert, "psf-interp", &SceneState::psfInterp, { "Nearest", "Linear", "Cubic", "Fft" },
                "Sub-pixel placement of the PSF (Fft: exact, slow, CPU only)."));
      d = Num(R, "PsfHaloCut", Tier::Expert, "psf-halo-cut", &SceneState::psfHaloCut, 0.0, 1e-3,
              "Blink splats leave out camera pixels below this share of the emitter's photons (Quality: Fast 1e-5, "
              "Realistic 3e-6, Exhaustive 0 = the whole kernel). WideField and other continuous dyes keep the whole kernel.");
      ThenNumber(d, quality);
      add(d);
      add(Int(R, "PsfPupilSamples", Tier::Expert, "psf-pupil-samples", &SceneState::psfPupilSamples, 0, 1024,
              "GibsonLanniZernike pupil samples per axis (0: as many as the kernel window needs; 64: webSMLM's, folds "
              "light back into a wide window)."));
      d = Row(R, "PsfGeneratorJavaHome", Tier::Expert, "java-home",
              "JRE/JDK root for the JVM PSF models (empty: auto-detect; read once per process).");
      d.kind = PropKind::Text;
      d.getText = [](H& h) { return h.State().JavaHome(); };
      d.setText = [](H& h, const std::string& s) {
         h.State().SetJavaHome(s);
         return true;
      };
      add(d);
      d = Arr(R, "WideFieldUpscaling", Tier::Expert, "wf-upscale", &SceneState::wideField, WF_UPSCALING, 1, 4,
              "Mean-field grid cells per pixel (WideField-mode populations).", true);
      ThenNumber(d, quality);
      add(d);
      add(Arr(R, "WideFieldZPlaneNm", Tier::Expert, "wf-plane-nm", &SceneState::wideField, WF_Z_PLANE_NM, 5, 500,
              "Mean-field dye planes, nm apart."));
      d = Arr(R, "BrightFieldQuality", Tier::Expert, "bf-quality", &SceneState::brightField, BF_QUALITY, 1, 4,
              "BrightField level 1-4 (sources, samples, slice step unless set below).", true);
      ThenNumber(d, quality);
      add(d);
      add(Arr(R, "BrightFieldSources", Tier::Expert, "bf-sources", &SceneState::brightField, BF_SOURCES, 0, 1024,
              "Condenser source points (0 = the quality's).", true));
      add(Arr(R, "BrightFieldUpscaling", Tier::Expert, "bf-upscale", &SceneState::brightField, BF_UPSCALING, 0, 8,
              "BrightField grid cells per pixel, a minimum (0 = the quality's).", true));
      add(Arr(R, "BrightFieldGeometrySamples", Tier::Expert, "bf-samples", &SceneState::brightField, BF_GEOMETRY_SAMPLES,
              0, 16, "Geometry samples per grid cell (0 = the quality's).", true));
      add(Arr(R, "BrightFieldSliceUm", Tier::Expert, "bf-slice", &SceneState::brightField, BF_SLICE_UM, -1, 5,
              "Multislice step, um (0 = one thin screen, -1 = the quality's)."));
      add(Opt(R, "MeanFieldDensityPerUm2", Tier::Expert, "mean-field-density-per-um2", 0, 1e6,
              "Continuous populations denser than this render mean-field."));
      add(Opt(R, "MeanFieldSlabNm", Tier::Expert, "mean-field-slab-nm", 0, 10000, "The density's z slab, nm."));
      add(Opt(R, "MeanFieldMaxEmitters", Tier::Expert, "mean-field-max-emitters", 0, 1e9,
              "Populations with more emitters than this render mean-field."));
      // The blink render regimes (spec/ALGORITHM.md "Blink render regimes"): splat, binned FFT, mean-field.
      add(Opt(R, "BlinkBinnedDensityPerUm2", Tier::Expert, "blink-binned-density-per-um2", 0, 1e9,
              "A frame's blinks render binned (the same blinks on a sub-pixel grid, FFT-convolved: approximate "
              "SMLM, the cost per FOV area instead of per blink) above this many ON emitters per um^2 of the FOV."));
      add(Opt(R, "BlinkBinnedMaxEmitters", Tier::Expert, "blink-binned-max-emitters", 0, 1e9,
              "And binned above this many ON emitters in the frame."));
      add(Opt(R, "BlinkBinnedUpscale", Tier::Expert, "blink-binned-upscale", 1, 8,
              "Cells of the binned grid per pixel, per axis (a blink is snapped to +-half a cell)."));
      add(Opt(R, "BlinkMeanFieldDensityPerUm2", Tier::Expert, "blink-mean-field-density-per-um2", 0, 1e9,
              "A frame's blinks render mean-field (no blinks drawn: each dye's expected ON time; the blinking "
              "itself is lost) above this many expected ON emitters per um^2 of the focal slab. 1e9 = never."));
      add(Opt(R, "BlinkMeanFieldMaxEmitters", Tier::Expert, "blink-mean-field-max-emitters", 0, 1e9,
              "And mean-field above this many expected ON emitters in the z range. 1e9 = never."));
      d = Row(R, "WriteScopeSpecTo", Tier::Expert, "write-spec",
              "Set to a file path: writes the engine spec of the current settings and stage pose there "
              "(insiliscope_cli --spec reproduces the frames).");
      d.kind = PropKind::Text;
      d.invalidate = Invalidate::None;
      d.getText = [](H&) { return std::string(); };
      d.setText = [](H& h, const std::string& path) {
         if (path.empty())
            return true;
         double x = 0, y = 0;
         h.Stage().PositionXyAt(sim::SharedStageState::Clock::now(), x, y);
         sim::ScopeSpec spec = BuildScopeSpec(h.State(), x, y, h.Stage().zPositionUm.load(), 0.0, 1);
         AddDriftToSpec(spec, SnapshotParams(h.State()).drift);
         std::ofstream f(path, std::ios::binary);   // plain LF line ends on every platform
         if (!f)
            return false;
         f << ScopeSpecText(spec);
         return static_cast<bool>(f);
      };
      add(d);
      // The registry as JSON, every tier (tools/gen_property_reference.py
      // writes docs/mm-properties.md from it); values are the current ones,
      // the defaults on a fresh hub.
      d = Row(R, "Test_WriteRegistryTo", Tier::Test, "write-registry",
              "Set to a file path: writes every row of the property registry there as JSON (the reference docs).");
      d.kind = PropKind::Text;
      d.invalidate = Invalidate::None;
      d.getText = [](H&) { return std::string(); };
      d.setText = [](H& h, const std::string& path) {
         if (path.empty())
            return true;
         auto q = [](const std::string& v) {
            std::string o = "\"";
            for (char c : v)
            {
               if (c == '"' || c == '\\')
                  o += '\\';
               if (static_cast<unsigned char>(c) >= 0x20)
                  o += c;
            }
            return o + "\"";
         };
         static const char* const kKinds[] = { "float", "integer", "text" };
         std::ofstream f(path);
         if (!f)
            return false;
         f << "[\n";
         bool first = true;
         for (const PropDef& r : PropertyTable())
         {
            f << (first ? "" : ",\n") << "{\"device\":" << q(r.device) << ",\"name\":" << q(r.name)
              << ",\"tier\":" << q(TierName(r.tier)) << ",\"kind\":" << q(kKinds[static_cast<int>(r.kind)])
              << ",\"readOnly\":" << (r.readOnly ? "true" : "false") << ",\"key\":" << q(r.key)
              << ",\"help\":" << q(r.help);
            if (r.lo < r.hi)
               f << ",\"lo\":" << q(FormatNumber(r.lo)) << ",\"hi\":" << q(FormatNumber(r.hi));
            if (r.getText)
               f << ",\"value\":" << q(r.getText(h));
            else if (r.get)
               f << ",\"value\":" << q(FormatNumber(r.get(h)));
            if (r.choices)
            {
               f << ",\"choices\":[";
               const std::vector<std::string> cs = r.choices(h);
               for (size_t i = 0; i < cs.size(); ++i)
                  f << (i ? "," : "") << q(cs[i]);
               f << "]";
            }
            f << "}";
            first = false;
         }
         f << "\n]\n";
         return static_cast<bool>(f);
      };
      add(d);
   }
   return t;
}

} // namespace

const std::vector<PropDef>& PropertyTable()
{
   static const std::vector<PropDef> table = BuildTable();
   return table;
}

} // namespace isc
