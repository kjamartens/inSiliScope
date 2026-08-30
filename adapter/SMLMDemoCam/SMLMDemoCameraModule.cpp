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
#include "ModuleInterface.h"

#include <cstring>

extern const char* g_SMLMCameraDeviceName;

MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_SMLMCameraDeviceName, MM::CameraDevice, "Synthetic SMLM demo camera");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == nullptr)
      return nullptr;

   if (strcmp(deviceName, g_SMLMCameraDeviceName) == 0)
      return new CSMLMDemoCamera();

   return nullptr;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}
