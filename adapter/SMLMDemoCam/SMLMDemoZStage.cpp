///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMDemoZStage.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   See SMLMDemoZStage.h.
//
// LICENSE:       BSD (see license.txt)

#include "SMLMDemoZStage.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>

const char* g_SMLMZStageDeviceName = "SMLMDemoZStage";

SMLMDemoZStage::SMLMDemoZStage()
{
   InitializeDefaultErrorMessages();
}

SMLMDemoZStage::~SMLMDemoZStage()
{
   Shutdown();
}

void SMLMDemoZStage::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_SMLMZStageDeviceName);
}

int SMLMDemoZStage::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   int ret = CreateStringProperty(MM::g_Keyword_Name, g_SMLMZStageDeviceName, true);
   if (ret != DEVICE_OK)
      return ret;

   ret = CreateStringProperty(MM::g_Keyword_Description,
                               "Global focus offset for SMLMDemoCam's vectorial PSF renderer", true);
   if (ret != DEVICE_OK)
      return ret;

   CPropertyAction* pAct = new CPropertyAction(this, &SMLMDemoZStage::OnPosition);
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

int SMLMDemoZStage::Shutdown()
{
   initialized_ = false;
   return DEVICE_OK;
}

int SMLMDemoZStage::SetPositionUm(double pos)
{
   pos = std::min(std::max(pos, kLowerLimitUm), kUpperLimitUm);
   sim::GetSharedStageState().zPositionUm.store(pos);
   return OnStagePositionChanged(pos);
}

int SMLMDemoZStage::GetPositionUm(double& pos)
{
   pos = sim::GetSharedStageState().zPositionUm.load();
   return DEVICE_OK;
}

int SMLMDemoZStage::SetPositionSteps(long steps)
{
   return SetPositionUm(steps * kStepSizeUm);
}

int SMLMDemoZStage::GetPositionSteps(long& steps)
{
   double pos;
   GetPositionUm(pos);
   steps = std::lround(pos / kStepSizeUm);
   return DEVICE_OK;
}

int SMLMDemoZStage::SetOrigin()
{
   return DEVICE_OK;
}

int SMLMDemoZStage::GetLimits(double& lower, double& upper)
{
   lower = kLowerLimitUm;
   upper = kUpperLimitUm;
   return DEVICE_OK;
}

int SMLMDemoZStage::Home()
{
   return SetPositionUm(0.0);
}

int SMLMDemoZStage::Stop()
{
   return DEVICE_OK;
}

int SMLMDemoZStage::OnPosition(MM::PropertyBase* pProp, MM::ActionType eAct)
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
