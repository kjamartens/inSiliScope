///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliCellScopeModule.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Module initialization and device factory for the synthetic
//                SMLM demo camera adapter.
//
// LICENSE:       BSD (see license.txt)

#include "InSiliCellScopeCamera.h"
#include "InSiliCellScopeXYStage.h"
#include "InSiliCellScopeZStage.h"
#include "ModuleInterface.h"

#include <cstring>

extern const char* g_CameraDeviceName;
extern const char* g_ZStageDeviceName;
extern const char* g_XYStageDeviceName;

MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_CameraDeviceName, MM::CameraDevice, "Synthetic SMLM demo camera");
   RegisterDevice(g_ZStageDeviceName, MM::StageDevice, "Global focus offset for inSiliCellScope");
   RegisterDevice(g_XYStageDeviceName, MM::XYStageDevice, "Moves inSiliCellScope's FOV over the cell field");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == nullptr)
      return nullptr;

   if (strcmp(deviceName, g_CameraDeviceName) == 0)
      return new CInSiliCellScopeCamera();

   if (strcmp(deviceName, g_ZStageDeviceName) == 0)
      return new InSiliCellScopeZStage();

   if (strcmp(deviceName, g_XYStageDeviceName) == 0)
      return new InSiliCellScopeXYStage();

   return nullptr;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}
