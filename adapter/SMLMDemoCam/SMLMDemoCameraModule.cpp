///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMDemoCameraModule.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Module initialization and device factory for the synthetic
//                SMLM demo camera adapter.
//
// LICENSE:       BSD (see license.txt)

#include "SMLMDemoCamera.h"
#include "SMLMDemoXYStage.h"
#include "SMLMDemoZStage.h"
#include "ModuleInterface.h"

#include <cstring>

extern const char* g_SMLMCameraDeviceName;
extern const char* g_SMLMZStageDeviceName;
extern const char* g_SMLMXYStageDeviceName;

MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_SMLMCameraDeviceName, MM::CameraDevice, "Synthetic SMLM demo camera");
   RegisterDevice(g_SMLMZStageDeviceName, MM::StageDevice, "Global focus offset for SMLMDemoCam");
   RegisterDevice(g_SMLMXYStageDeviceName, MM::XYStageDevice, "Moves SMLMDemoCam's FOV over the cell field");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == nullptr)
      return nullptr;

   if (strcmp(deviceName, g_SMLMCameraDeviceName) == 0)
      return new CSMLMDemoCamera();

   if (strcmp(deviceName, g_SMLMZStageDeviceName) == 0)
      return new SMLMDemoZStage();

   if (strcmp(deviceName, g_SMLMXYStageDeviceName) == 0)
      return new SMLMDemoXYStage();

   return nullptr;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}
