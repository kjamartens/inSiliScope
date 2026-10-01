///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeXYStage.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   See InSiliScopeXYStage.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeXYStage.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>

const char* g_XYStageDeviceName = "XYStage";
const char* g_PropStageSpeedUmPerSec = "General_StageSpeedUmPerSec";
const char* g_PropStageSettleMs = "General_StageSettleMs";
const char* g_PropStageLimitUm = "General_StageLimitUm";

InSiliScopeXYStage::InSiliScopeXYStage()
{
   InitializeDefaultErrorMessages();
}

InSiliScopeXYStage::~InSiliScopeXYStage()
{
   Shutdown();
}

void InSiliScopeXYStage::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_XYStageDeviceName);
}

int InSiliScopeXYStage::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   int ret = CreateStringProperty(MM::g_Keyword_Name, g_XYStageDeviceName, true);
   if (ret != DEVICE_OK)
      return ret;
   ret = CreateStringProperty(MM::g_Keyword_Description,
                              "Moves inSiliScope's field of view over the CellField pattern", true);
   if (ret != DEVICE_OK)
      return ret;

   sim::SharedStageState& st = sim::GetSharedStageState();
   ret = CreateFloatProperty(g_PropStageSpeedUmPerSec, st.XySpeed(), false,
                             new CPropertyAction(this, &InSiliScopeXYStage::OnSpeed));
   if (ret != DEVICE_OK)
      return ret;
   SetPropertyLimits(g_PropStageSpeedUmPerSec, 1.0, 100000.0);
   ret = CreateFloatProperty(g_PropStageSettleMs, st.XySettleSec() * 1000.0, false,
                             new CPropertyAction(this, &InSiliScopeXYStage::OnSettleMs));
   if (ret != DEVICE_OK)
      return ret;
   SetPropertyLimits(g_PropStageSettleMs, 0.0, 10000.0);
   ret = CreateFloatProperty(g_PropStageLimitUm, limitUm_, false,
                             new CPropertyAction(this, &InSiliScopeXYStage::OnLimitUm));
   if (ret != DEVICE_OK)
      return ret;

   initialized_ = true;
   return DEVICE_OK;
}

int InSiliScopeXYStage::Shutdown()
{
   initialized_ = false;
   return DEVICE_OK;
}

bool InSiliScopeXYStage::Busy()
{
   return sim::GetSharedStageState().XyBusy();
}

int InSiliScopeXYStage::SetPositionSteps(long x, long y)
{
   const double lim = limitUm_;
   const double xUm = std::min(std::max(x * kStepSizeUm, -lim), lim);
   const double yUm = std::min(std::max(y * kStepSizeUm, -lim), lim);
   sim::GetSharedStageState().SetXyTarget(xUm, yUm);
   return DEVICE_OK;
}

int InSiliScopeXYStage::GetPositionSteps(long& x, long& y)
{
   double xUm, yUm;
   sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), xUm, yUm);
   x = std::lround(xUm / kStepSizeUm);
   y = std::lround(yUm / kStepSizeUm);
   return DEVICE_OK;
}

int InSiliScopeXYStage::SetRelativePositionSteps(long x, long y)
{
   long cx, cy;
   GetPositionSteps(cx, cy);
   return SetPositionSteps(cx + x, cy + y);
}

int InSiliScopeXYStage::Home()
{
   return SetPositionSteps(0, 0);
}

int InSiliScopeXYStage::Stop()
{
   sim::GetSharedStageState().StopXy();
   return DEVICE_OK;
}

int InSiliScopeXYStage::SetOrigin()
{
   // Only the reported coordinates change; the FOV stays where it is.
   return SetAdapterOriginUm(0.0, 0.0);
}

int InSiliScopeXYStage::GetLimitsUm(double& xMin, double& xMax, double& yMin, double& yMax)
{
   xMin = yMin = -limitUm_;
   xMax = yMax = limitUm_;
   return DEVICE_OK;
}

int InSiliScopeXYStage::GetStepLimits(long& xMin, long& xMax, long& yMin, long& yMax)
{
   const long lim = static_cast<long>(std::min(limitUm_ / kStepSizeUm, 2.0e9));
   xMin = yMin = -lim;
   xMax = yMax = lim;
   return DEVICE_OK;
}

int InSiliScopeXYStage::OnSpeed(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(sim::GetSharedStageState().XySpeed());
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      sim::GetSharedStageState().SetXySpeed(v);
   }
   return DEVICE_OK;
}

int InSiliScopeXYStage::OnSettleMs(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(sim::GetSharedStageState().XySettleSec() * 1000.0);
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      sim::GetSharedStageState().SetXySettleSec(v / 1000.0);
   }
   return DEVICE_OK;
}

int InSiliScopeXYStage::OnLimitUm(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
      pProp->Set(limitUm_);
   else if (eAct == MM::AfterSet)
   {
      double v;
      pProp->Get(v);
      limitUm_ = std::max(1.0, std::min(v, 1.0e7));
   }
   return DEVICE_OK;
}
