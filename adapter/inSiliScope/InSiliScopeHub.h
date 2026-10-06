///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeHub.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The inSiliScope hub: the simulated microscope's shared state.
//                It owns every setting (Registry/SceneState.h) and the stage
//                state; the peripherals (camera, stages, objective, filters,
//                light sources, sample holder, specimen, fluorophores,
//                renderer) are its children and read and write it through
//                their properties (Registry/PropertyTable.cpp). Pre-init:
//                Detail (which tiers of properties the session shows) and
//                RandomSeed. Couplings across devices (a preset setting its
//                members, a dye pick loading the light path) go through the
//                hub: it changes the settings and tells every device which
//                ones changed (Notify).
//
//                Device and tier rules: spec/MM_DEVICES.md.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "DeviceBase.h"
#include "Registry/PropertyTable.h"
#include "Registry/SceneState.h"
#include "Simulation/SharedStageState.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern const char* g_HubDeviceName;

namespace isc {

// A device with registry rows: told when a setting it shows changed.
class RowSink
{
public:
   virtual ~RowSink() {}
   // A coupling changed the setting `key`: refresh the rows bound to it.
   virtual void KeyChanged(const std::string& key) = 0;
   // The allowed values of the rows bound to `key` changed.
   virtual void ChoicesChanged(const std::string& key) = 0;
};

// The peripherals and their device tiers (DetectInstalledDevices offers the
// ones at or below the session's Detail).
struct DeviceInfo
{
   const char* name;
   const char* description;
   Tier tier;
};
const std::vector<DeviceInfo>& Peripherals();

} // namespace isc

class InSiliScopeHub : public HubBase<InSiliScopeHub>
{
public:
   InSiliScopeHub();
   ~InSiliScopeHub();

   // MMDevice API
   int Initialize();
   int Shutdown();
   void GetName(char* name) const;
   bool Busy() { return false; }

   // Hub API
   int DetectInstalledDevices();

   int OnDetail(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnRandomSeed(MM::PropertyBase* pProp, MM::ActionType eAct);

   // ---- shared state ----
   isc::SceneState& State() { return state_; }
   sim::SharedStageState& Stage() { return stage_; }
   // The tiers this session shows; the Test rows exist only with ISC_TEST=1.
   isc::Tier Detail() const { return detail_; }
   bool TestRows() const { return testRows_; }
   bool Shows(isc::Tier t) const
   {
      return t == isc::Tier::Test ? testRows_ : static_cast<int>(t) <= static_cast<int>(detail_);
   }

   // ---- devices ----
   void AddSink(isc::RowSink* sink);
   void RemoveSink(isc::RowSink* sink);
   // Refresh every row bound to `key` (on every device).
   void Notify(const std::string& key);
   void RefreshChoices(const std::string& key);
   // The camera: told what a change invalidates.
   void SetInvalidateListener(std::function<void(isc::Invalidate)> f);
   void Changed(isc::Invalidate what);
   // A light source's shutter: its state is part of every frame.
   void SetLight(bool epi, bool open);
   void Log(const std::string& msg, bool debugOnly = true) { LogMessage(msg, debugOnly); }

   // ---- couplings (Registry/Couplings.cpp) ----
   // A dye pick or label mode change: the labelled sites take the mode's
   // suggestion (on a mode change), the camera its mode gain, the light path
   // the dye mode's light preset.
   void LoadMicrotubuleDye(bool modeMayChange);
   void ApplyLightPreset(const std::string& id);
   // Lasers or filters set by hand: the light path is no preset's any more.
   void ClearLightPreset();
   void ApplyCameraPreset(int index);
   void ApplyModeGain();
   void ApplyZernikePreset(const std::string& name);
   // A dye slot's new source: its edits go; the microtubules reload if they use it.
   void DyeSlotSourceChanged(int slot);

private:
   bool initialized_ = false;
   isc::Tier detail_ = isc::Tier::Basic;
   bool testRows_ = false;
   isc::SceneState state_;
   sim::SharedStageState stage_;
   std::mutex sinkMutex_;
   std::vector<isc::RowSink*> sinks_;
   std::function<void(isc::Invalidate)> invalidate_;
};
