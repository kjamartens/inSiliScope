///////////////////////////////////////////////////////////////////////////////
// FILE:          Couplings.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The couplings between settings (InSiliScopeHub.h): a preset
//                sets its members, a dye pick loads its mode's labelling and
//                light path, a camera preset its noise values. Each changes
//                the settings, then tells every device which ones changed
//                (InSiliScopeHub::Notify), so the property browser shows the
//                new values on whichever device they live.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "../InSiliScopeHub.h"

#include "../Simulation/DyeLibrary.h"
#include "../Simulation/SMLMZernike.h"

#include <cmath>
#include <cstring>

void InSiliScopeHub::LoadMicrotubuleDye(bool modeMayChange)
{
   using namespace sim;
   isc::SceneState& st = state_;
   st.ClearDyeEditsWithPrefix("mt-dye.");
   std::vector<DyeSlot> slots(3);
   for (int n = 0; n < 3; ++n)
      slots[static_cast<size_t>(n)].source = static_cast<int>(st.Option("dye" + std::to_string(n + 1) + ".source"));
   EffectiveDye eff;
   std::string err;
   if (!MakeEffectiveDye(static_cast<int>(st.Option("mt-dye")), slots, {}, static_cast<int>(st.Option("mt-mode")), eff,
                         err))
   {
      Log("Microtubule dye: " + err, false);
      return;
   }
   const bool modeChanged = eff.mode != st.lastMtMode.load();
   if (modeMayChange && modeChanged)
   {
      st.SetOption("mt-label-pct", SuggestedLabelingPct(eff.mode));
      Notify("mt-label-pct");
   }
   st.lastMtMode = eff.mode;
   if (modeChanged)
      ApplyModeGain();
   if (eff.dye.modes[eff.mode].lightPreset)
      ApplyLightPreset(eff.dye.modes[eff.mode].lightPreset);
   for (int i = 0; i < 3; ++i)
      Notify("mt-readout-" + std::to_string(i));
}

void InSiliScopeHub::ApplyLightPreset(const std::string& id)
{
   const sim::LightPresetData* q = sim::FindLightPreset(id);
   if (!q)
      return;
   const std::vector<int>& lines = sim::LaserLines();
   for (size_t l = 0; l < lines.size(); ++l)
      state_.SetOption("laser-" + std::to_string(lines[l]), q->lasers[l]);
   state_.SetOption("dichroic", sim::IndexOf(sim::DichroicIds(), q->dichroic));
   state_.SetOption("em-filter", sim::IndexOf(sim::EmissionFilterIds(), q->emissionFilter));
   state_.SetLightPreset(id);
   for (size_t l = 0; l < lines.size(); ++l)
      Notify("laser-" + std::to_string(lines[l]));
   Notify("dichroic");
   Notify("em-filter");
   Notify("light-preset");
}

void InSiliScopeHub::ClearLightPreset()
{
   if (state_.LightPreset() == "None")
      return;
   state_.SetLightPreset("None");
   Notify("light-preset");
}

void InSiliScopeHub::ApplyCameraPreset(int index)
{
   if (index < 0 || index >= static_cast<int>(sim::CameraIds().size()))
      return;
   const sim::CameraData& c = sim::CameraAt(index);
   isc::SceneState& st = state_;
   st.cameraPreset = index;
   auto set = [&](std::atomic<double>& member, sim::CameraField f, const char* key) {
      if (!std::isnan(c.v[f]))
      {
         member = c.v[f];
         Notify(key);
      }
   };
   set(st.quantumEfficiency, sim::CAM_QE, "qe");
   set(st.readNoiseElectrons, sim::CAM_READ_NOISE, "read-noise");
   set(st.cicElectrons, sim::CAM_CIC, "cic");
   set(st.offsetAdu, sim::CAM_OFFSET, "offset");
   set(st.offsetStdAdu, sim::CAM_OFFSET_STD, "offset-std");
   set(st.darkCurrentPerSec, sim::CAM_DARK, "dark-per-sec");
   set(st.pixelGainStdPct, sim::CAM_GAIN_STD_PCT, "gain-std-pct");
   set(st.pixelReadNoiseStdPct, sim::CAM_READ_NOISE_STD_PCT, "read-noise-std-pct");
   set(st.sensorPixelUm, sim::CAM_PIXEL_UM, "sensor-pixel-um");
   Notify("pixel-nm");
   if (!std::isnan(c.v[sim::CAM_BIT_DEPTH]))
   {
      st.bitDepth = static_cast<int>(c.v[sim::CAM_BIT_DEPTH]);
      Notify("bit-depth");
   }
   if (c.type)
   {
      st.emccd = !std::strcmp(c.type, "EMCCD") ? 1 : 0;
      Notify("camera-type");
   }
   st.SetOption("qe-curve", index);
   Notify("qe-curve");
   const double gain =
      sim::CameraPresetGain(c, (st.transOpen.load() && !st.epiOpen.load()) ||
                                  st.lastMtMode.load() == sim::IndexOf(sim::DyeModeNames(), "WideField"));
   if (!std::isnan(gain))
   {
      st.gainElectronsPerAdu = gain;
      Notify("gain");
   }
   Notify("em-gain");
   Notify("camera-preset");
}

void InSiliScopeHub::ApplyModeGain()
{
   isc::SceneState& st = state_;
   const int p = st.cameraPreset.load();
   if (p < 0 || p >= static_cast<int>(sim::CameraIds().size()))
      return;
   const sim::CameraData& c = sim::CameraAt(p);
   if (std::isnan(c.v[sim::CAM_GAIN_WF]))
      return;
   st.gainElectronsPerAdu =
      sim::CameraPresetGain(c, (st.transOpen.load() && !st.epiOpen.load()) ||
                                  st.lastMtMode.load() == sim::IndexOf(sim::DyeModeNames(), "WideField"));
   Notify("gain");
   Notify("em-gain");
}

void InSiliScopeHub::ApplyZernikePreset(const std::string& name)
{
   state_.SetZernikePreset(name);
   state_.SetZernikeCoefficients(sim::FormatZernikeCoefficients(sim::ZernikePresetCoefficients(name)));
   Notify("zernike");
}

void InSiliScopeHub::DyeSlotSourceChanged(int slot)
{
   const std::string prefix = "dye" + std::to_string(slot + 1);
   state_.ClearDyeEditsWithPrefix(prefix + ".");
   if (static_cast<int>(state_.Option("mt-dye")) == static_cast<int>(sim::DyeIds().size()) + slot)
      LoadMicrotubuleDye(true);
   Notify(prefix + ".fields");
}
