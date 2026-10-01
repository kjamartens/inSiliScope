///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeModule.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Module initialization and device factory for the synthetic
//                SMLM demo camera adapter.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeCamera.h"
#include "InSiliScopeXYStage.h"
#include "InSiliScopeZStage.h"
#include "ModuleInterface.h"

#include <cstring>

extern const char* g_CameraDeviceName;
extern const char* g_ZStageDeviceName;
extern const char* g_XYStageDeviceName;

MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_CameraDeviceName, MM::CameraDevice, "Synthetic SMLM demo camera");
   RegisterDevice(g_ZStageDeviceName, MM::StageDevice, "Global focus offset for inSiliScope");
   RegisterDevice(g_XYStageDeviceName, MM::XYStageDevice, "Moves inSiliScope's FOV over the cell field");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == nullptr)
      return nullptr;

   if (strcmp(deviceName, g_CameraDeviceName) == 0)
      return new CInSiliScopeCamera();

   if (strcmp(deviceName, g_ZStageDeviceName) == 0)
      return new InSiliScopeZStage();

   if (strcmp(deviceName, g_XYStageDeviceName) == 0)
      return new InSiliScopeXYStage();

   return nullptr;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}
