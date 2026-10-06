///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeModule.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Module initialization and device factory: the inSiliScope
//                hub and its peripherals (InSiliScopeHub.h; which device owns
//                what: spec/MM_DEVICES.md).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Devices/Peripherals.h"
#include "InSiliScopeCamera.h"
#include "InSiliScopeHub.h"
#include "InSiliScopeXYStage.h"
#include "InSiliScopeZStage.h"
#include "ModuleInterface.h"

#include <cstring>

MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_HubDeviceName, MM::HubDevice, "inSiliScope: a simulated microscope (load this hub first)");
   for (const isc::DeviceInfo& d : isc::Peripherals())
   {
      MM::DeviceType type = MM::GenericDevice;
      if (!std::strcmp(d.name, "Camera"))
         type = MM::CameraDevice;
      else if (!std::strcmp(d.name, "XYStage"))
         type = MM::XYStageDevice;
      else if (!std::strcmp(d.name, "ZStage"))
         type = MM::StageDevice;
      else if (!std::strcmp(d.name, "Objective") || !std::strcmp(d.name, "FilterCube") ||
               !std::strcmp(d.name, "ExcitationFilter") || !std::strcmp(d.name, "Dichroic") || !std::strcmp(d.name, "EmissionFilter") ||
               !std::strcmp(d.name, "SampleHolder"))
         type = MM::StateDevice;
      else if (!std::strcmp(d.name, "Lasers") || !std::strcmp(d.name, "TransmittedLamp"))
         type = MM::ShutterDevice;
      else if (!std::strcmp(d.name, "EmissionPath"))
         type = MM::MagnifierDevice;
      RegisterDevice(d.name, type, d.description);
   }
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == nullptr)
      return nullptr;
   const std::string n = deviceName;
   if (n == g_HubDeviceName)
      return new InSiliScopeHub();
   if (n == g_CameraDeviceName)
      return new CInSiliScopeCamera();
   if (n == g_XYStageDeviceName)
      return new InSiliScopeXYStage();
   if (n == g_ZStageDeviceName)
      return new InSiliScopeZStage();
   if (n == "Objective")
      return new isc::ObjectiveDevice();
   if (n == "EmissionPath")
      return new isc::EmissionPathDevice();
   if (n == "FilterCube")
      return new isc::FilterCubeDevice();
   if (n == "ExcitationFilter")
      return new isc::FilterWheelDevice("ExcitationFilter",
                                        "Laser clean-up filter wheel (the cube's excitation filter, set on its own)",
                                        "ex-filter");
   if (n == "Dichroic")
      return new isc::FilterWheelDevice("Dichroic", "Dichroic mirror wheel (the cube's dichroic, set on its own)",
                                        "dichroic");
   if (n == "EmissionFilter")
      return new isc::FilterWheelDevice("EmissionFilter",
                                        "Emission filter wheel (the cube's emission filter, set on its own)",
                                        "em-filter");
   if (n == "Lasers")
      return new isc::LasersDevice();
   if (n == "TransmittedLamp")
      return new isc::TransmittedLampDevice();
   if (n == "SampleHolder")
      return new isc::SampleHolderDevice();
   if (n == "CellField")
      return new isc::CellFieldDevice();
   if (n == "Fluorophores")
      return new isc::FluorophoresDevice();
   if (n == "Renderer")
      return new isc::RendererDevice();
   return nullptr;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}
