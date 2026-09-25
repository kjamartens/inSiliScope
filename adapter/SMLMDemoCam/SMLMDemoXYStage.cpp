///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMDemoXYStage.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   See SMLMDemoXYStage.h.
//
// LICENSE:       BSD (see license.txt)

#include "SMLMDemoXYStage.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>

const char* g_SMLMXYStageDeviceName = "SMLMDemoXYStage";
const char* g_PropStageSpeedUmPerSec = "General_StageSpeedUmPerSec";
const char* g_PropStageSettleMs = "General_StageSettleMs";
const char* g_PropStageLimitUm = "General_StageLimitUm";

SMLMDemoXYStage::SMLMDemoXYStage()
{
   InitializeDefaultErrorMessages();
}

SMLMDemoXYStage::~SMLMDemoXYStage()
{
   Shutdown();
}

void SMLMDemoXYStage::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_SMLMXYStageDeviceName);
}

int SMLMDemoXYStage::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   int ret = CreateStringProperty(MM::g_Keyword_Name, g_SMLMXYStageDeviceName, true);
   if (ret != DEVICE_OK)
      return ret;
   ret = CreateStringProperty(MM::g_Keyword_Description,
                              "Moves SMLMDemoCam's field of view over the CellField pattern", true);
   if (ret != DEVICE_OK)
      return ret;

   sim::SharedStageState& st = sim::GetSharedStageState();
   ret = CreateFloatProperty(g_PropStageSpeedUmPerSec, st.XySpeed(), false,
                             new CPropertyAction(this, &SMLMDemoXYStage::OnSpeed));
   if (ret != DEVICE_OK)
      return ret;
   SetPropertyLimits(g_PropStageSpeedUmPerSec, 1.0, 100000.0);
   ret = CreateFloatProperty(g_PropStageSettleMs, st.XySettleSec() * 1000.0, false,
                             new CPropertyAction(this, &SMLMDemoXYStage::OnSettleMs));
   if (ret != DEVICE_OK)
      return ret;
   SetPropertyLimits(g_PropStageSettleMs, 0.0, 10000.0);
   ret = CreateFloatProperty(g_PropStageLimitUm, limitUm_, false,
                             new CPropertyAction(this, &SMLMDemoXYStage::OnLimitUm));
   if (ret != DEVICE_OK)
      return ret;

   initialized_ = true;
   return DEVICE_OK;
}

int SMLMDemoXYStage::Shutdown()
{
   initialized_ = false;
   return DEVICE_OK;
}

bool SMLMDemoXYStage::Busy()
{
   return sim::GetSharedStageState().XyBusy();
}

int SMLMDemoXYStage::SetPositionSteps(long x, long y)
{
   const double lim = limitUm_;
   const double xUm = std::min(std::max(x * kStepSizeUm, -lim), lim);
   const double yUm = std::min(std::max(y * kStepSizeUm, -lim), lim);
   sim::GetSharedStageState().SetXyTarget(xUm, yUm);
   return DEVICE_OK;
}

int SMLMDemoXYStage::GetPositionSteps(long& x, long& y)
{
   double xUm, yUm;
   sim::GetSharedStageState().PositionXyAt(sim::SharedStageState::Clock::now(), xUm, yUm);
   x = std::lround(xUm / kStepSizeUm);
   y = std::lround(yUm / kStepSizeUm);
   return DEVICE_OK;
}

int SMLMDemoXYStage::SetRelativePositionSteps(long x, long y)
{
   long cx, cy;
   GetPositionSteps(cx, cy);
   return SetPositionSteps(cx + x, cy + y);
}

int SMLMDemoXYStage::Home()
{
   return SetPositionSteps(0, 0);
}

int SMLMDemoXYStage::Stop()
{
   sim::GetSharedStageState().StopXy();
   return DEVICE_OK;
}

int SMLMDemoXYStage::SetOrigin()
{
   // Only the reported coordinates change; the FOV stays where it is.
   return SetAdapterOriginUm(0.0, 0.0);
}

int SMLMDemoXYStage::GetLimitsUm(double& xMin, double& xMax, double& yMin, double& yMax)
{
   xMin = yMin = -limitUm_;
   xMax = yMax = limitUm_;
   return DEVICE_OK;
}

int SMLMDemoXYStage::GetStepLimits(long& xMin, long& xMax, long& yMin, long& yMax)
{
   const long lim = static_cast<long>(std::min(limitUm_ / kStepSizeUm, 2.0e9));
   xMin = yMin = -lim;
   xMax = yMax = lim;
   return DEVICE_OK;
}

int SMLMDemoXYStage::OnSpeed(MM::PropertyBase* pProp, MM::ActionType eAct)
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

int SMLMDemoXYStage::OnSettleMs(MM::PropertyBase* pProp, MM::ActionType eAct)
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

int SMLMDemoXYStage::OnLimitUm(MM::PropertyBase* pProp, MM::ActionType eAct)
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
