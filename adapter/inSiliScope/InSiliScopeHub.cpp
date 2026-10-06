///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeHub.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The inSiliScope hub (InSiliScopeHub.h): the pre-init Detail
//                and RandomSeed, child discovery for the Hardware
//                Configuration Wizard, and the notifications between devices.
//                The couplings are in Registry/Couplings.cpp.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeHub.h"

#include "ModuleInterface.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

const char* g_HubDeviceName = "inSiliScope";

namespace isc {

const std::vector<DeviceInfo>& Peripherals()
{
   // In the Hardware Configuration Wizard's order: the light path from the
   // sample to the camera, then the simulation-side devices.
   static const std::vector<DeviceInfo> devices = {
      { "Camera", "The camera: sensor, noise chain, acquisition", Tier::Basic },
      { "XYStage", "Moves the field of view over the sample", Tier::Basic },
      { "ZStage", "The focus: height of the focal plane above the coverslip", Tier::Basic },
      { "Objective", "Objective turret: NA, immersion, magnification, aberrations", Tier::Basic },
      { "EmissionPath", "Magnification between objective and camera (sets the pixel size)", Tier::Basic },
      { "FilterCube", "Filter cube: dichroic and emission filter as one choice", Tier::Basic },
      { "Dichroic", "Dichroic mirror wheel (the cube's dichroic, set on its own)", Tier::Advanced },
      { "EmissionFilter", "Emission filter wheel (the cube's emission filter, set on its own)", Tier::Advanced },
      { "Lasers", "Laser engine and its shutter: line powers, beam profile, geometry", Tier::Basic },
      { "TransmittedLamp", "Transmitted-light lamp and its shutter (BrightField)", Tier::Basic },
      { "SampleHolder", "The mounted specimen; drift and background of the sample", Tier::Basic },
      { "CellField", "Specimen: a field of cells with their labelled structures", Tier::Basic },
      { "Fluorophores", "The dyes: label mode, photophysics readouts, custom dyes", Tier::Basic },
      { "Renderer", "How the images are computed: quality, GPU, caches, numerics", Tier::Basic },
   };
   return devices;
}

} // namespace isc

InSiliScopeHub::InSiliScopeHub()
{
   InitializeDefaultErrorMessages();
   const char* t = std::getenv("ISC_TEST");
   testRows_ = t && std::strcmp(t, "1") == 0;
   CreateStringProperty("Detail", isc::TierName(detail_), false,
                        new CPropertyAction(this, &InSiliScopeHub::OnDetail), true);
   AddAllowedValue("Detail", "Basic");
   AddAllowedValue("Detail", "Advanced");
   AddAllowedValue("Detail", "Expert");
   CreateIntegerProperty("RandomSeed", state_.seed.load(), false,
                         new CPropertyAction(this, &InSiliScopeHub::OnRandomSeed), true);
}

InSiliScopeHub::~InSiliScopeHub()
{
   Shutdown();
}

void InSiliScopeHub::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_HubDeviceName);
}

int InSiliScopeHub::Initialize()
{
   if (initialized_)
      return DEVICE_OK;
   CreateStringProperty(MM::g_Keyword_Name, g_HubDeviceName, true);
   CreateStringProperty(MM::g_Keyword_Description,
                        "inSiliScope: a simulated fluorescence/BrightField microscope (hub of its devices)", true);
   initialized_ = true;
   return DEVICE_OK;
}

int InSiliScopeHub::Shutdown()
{
   initialized_ = false;
   return DEVICE_OK;
}

int InSiliScopeHub::DetectInstalledDevices()
{
   ClearInstalledDevices();
   // The module's devices, created through the module interface (as
   // DemoCamera's hub does), those the session's Detail shows.
   InitializeModuleData();
   for (const isc::DeviceInfo& d : isc::Peripherals())
      if (Shows(d.tier))
         if (MM::Device* dev = CreateDevice(d.name))
            AddInstalledDevice(dev);
   return DEVICE_OK;
}

int InSiliScopeHub::OnDetail(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(isc::TierName(detail_));
   else if (eAct == MM::AfterSet)
   {
      std::string s;
      pProp->Get(s);
      detail_ = isc::TierFromName(s);
   }
   return DEVICE_OK;
}

int InSiliScopeHub::OnRandomSeed(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(state_.seed.load());
   else if (eAct == MM::AfterSet)
   {
      long v;
      pProp->Get(v);
      state_.seed = v;
      Changed(isc::Invalidate::All);
   }
   return DEVICE_OK;
}

void InSiliScopeHub::AddSink(isc::RowSink* sink)
{
   std::lock_guard<std::mutex> g(sinkMutex_);
   if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end())
      sinks_.push_back(sink);
}

void InSiliScopeHub::RemoveSink(isc::RowSink* sink)
{
   std::lock_guard<std::mutex> g(sinkMutex_);
   sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void InSiliScopeHub::Notify(const std::string& key)
{
   std::vector<isc::RowSink*> sinks;
   {
      std::lock_guard<std::mutex> g(sinkMutex_);
      sinks = sinks_;
   }
   for (isc::RowSink* s : sinks)
      s->KeyChanged(key);
}

void InSiliScopeHub::RefreshChoices(const std::string& key)
{
   std::vector<isc::RowSink*> sinks;
   {
      std::lock_guard<std::mutex> g(sinkMutex_);
      sinks = sinks_;
   }
   for (isc::RowSink* s : sinks)
      s->ChoicesChanged(key);
}

void InSiliScopeHub::SetInvalidateListener(std::function<void(isc::Invalidate)> f)
{
   std::lock_guard<std::mutex> g(sinkMutex_);
   invalidate_ = std::move(f);
}

void InSiliScopeHub::Changed(isc::Invalidate what)
{
   std::function<void(isc::Invalidate)> f;
   {
      std::lock_guard<std::mutex> g(sinkMutex_);
      f = invalidate_;
   }
   if (f && what != isc::Invalidate::None)
      f(what);
}

void InSiliScopeHub::SetLight(bool epi, bool open)
{
   std::atomic<bool>& light = epi ? state_.epiOpen : state_.transOpen;
   if (light.exchange(open) != open)
   {
      ++state_.lightVersion;
      // A camera preset whose gain depends on the imaging (BrightField).
      ApplyModeGain();
      Changed(isc::Invalidate::Stack);
   }
}
