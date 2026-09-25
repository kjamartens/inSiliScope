///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliCellScopeZStage.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   See InSiliCellScopeZStage.h.
//
// LICENSE:       BSD (see license.txt)

#include "InSiliCellScopeZStage.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>

const char* g_ZStageDeviceName = "ZStage";

InSiliCellScopeZStage::InSiliCellScopeZStage()
{
   InitializeDefaultErrorMessages();
}

InSiliCellScopeZStage::~InSiliCellScopeZStage()
{
   Shutdown();
}

void InSiliCellScopeZStage::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_ZStageDeviceName);
}

int InSiliCellScopeZStage::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   int ret = CreateStringProperty(MM::g_Keyword_Name, g_ZStageDeviceName, true);
   if (ret != DEVICE_OK)
      return ret;

   ret = CreateStringProperty(MM::g_Keyword_Description,
                               "Global focus offset for inSiliCellScope's vectorial PSF renderer", true);
   if (ret != DEVICE_OK)
      return ret;

   CPropertyAction* pAct = new CPropertyAction(this, &InSiliCellScopeZStage::OnPosition);
   ret = CreateFloatProperty(MM::g_Keyword_Position, sim::GetSharedStageState().zPositionUm.load(), false, pAct);
   if (ret != DEVICE_OK)
      return ret;
   SetPropertyLimits(MM::g_Keyword_Position, kLowerLimitUm, kUpperLimitUm);

   ret = UpdateStatus();
   if (ret != DEVICE_OK)
      return ret;

   initialized_ = true;
   return DEVICE_OK;
}

int InSiliCellScopeZStage::Shutdown()
{
   initialized_ = false;
   return DEVICE_OK;
}

int InSiliCellScopeZStage::SetPositionUm(double pos)
{
   pos = std::min(std::max(pos, kLowerLimitUm), kUpperLimitUm);
   sim::GetSharedStageState().zPositionUm.store(pos);
   return OnStagePositionChanged(pos);
}

int InSiliCellScopeZStage::GetPositionUm(double& pos)
{
   pos = sim::GetSharedStageState().zPositionUm.load();
   return DEVICE_OK;
}

int InSiliCellScopeZStage::SetPositionSteps(long steps)
{
   return SetPositionUm(steps * kStepSizeUm);
}

int InSiliCellScopeZStage::GetPositionSteps(long& steps)
{
   double pos;
   GetPositionUm(pos);
   steps = std::lround(pos / kStepSizeUm);
   return DEVICE_OK;
}

int InSiliCellScopeZStage::SetOrigin()
{
   return DEVICE_OK;
}

int InSiliCellScopeZStage::GetLimits(double& lower, double& upper)
{
   lower = kLowerLimitUm;
   upper = kUpperLimitUm;
   return DEVICE_OK;
}

int InSiliCellScopeZStage::Home()
{
   return SetPositionUm(0.0);
}

int InSiliCellScopeZStage::Stop()
{
   return DEVICE_OK;
}

int InSiliCellScopeZStage::OnPosition(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(sim::GetSharedStageState().zPositionUm.load());
   }
   else if (eAct == MM::AfterSet)
   {
      double pos;
      pProp->Get(pos);
      SetPositionUm(pos);
   }
   return DEVICE_OK;
}
