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

InSiliScopeXYStage::InSiliScopeXYStage()
{
   SetRegistryErrorTexts();
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

   // StageSpeedUmPerSec, StageSettleMs, StageLimitUm: the registry's rows.
   int ret = InitRegistry(g_XYStageDeviceName);
   if (ret != DEVICE_OK)
      return ret;
   ret = CreateStringProperty(MM::g_Keyword_Name, g_XYStageDeviceName, true);
   if (ret != DEVICE_OK)
      return ret;
   ret = CreateStringProperty(MM::g_Keyword_Description,
                              "Moves the field of view over the sample", true);
   if (ret != DEVICE_OK)
      return ret;

   initialized_ = true;
   return DEVICE_OK;
}

int InSiliScopeXYStage::Shutdown()
{
   ShutdownRegistry();
   initialized_ = false;
   return DEVICE_OK;
}

bool InSiliScopeXYStage::Busy()
{
   return Hub() && Hub()->Stage().XyBusy();
}

int InSiliScopeXYStage::SetPositionSteps(long x, long y)
{
   const double lim = LimitUm();
   const double xUm = std::min(std::max(x * kStepSizeUm, -lim), lim);
   const double yUm = std::min(std::max(y * kStepSizeUm, -lim), lim);
   Hub()->Stage().SetXyTarget(xUm, yUm);
   return DEVICE_OK;
}

int InSiliScopeXYStage::GetPositionSteps(long& x, long& y)
{
   double xUm, yUm;
   Hub()->Stage().PositionXyAt(sim::SharedStageState::Clock::now(), xUm, yUm);
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
   Hub()->Stage().StopXy();
   return DEVICE_OK;
}

int InSiliScopeXYStage::SetOrigin()
{
   // Only the reported coordinates change; the FOV stays where it is.
   return SetAdapterOriginUm(0.0, 0.0);
}

int InSiliScopeXYStage::GetLimitsUm(double& xMin, double& xMax, double& yMin, double& yMax)
{
   xMin = yMin = -LimitUm();
   xMax = yMax = LimitUm();
   return DEVICE_OK;
}

int InSiliScopeXYStage::GetStepLimits(long& xMin, long& xMax, long& yMin, long& yMax)
{
   const long lim = static_cast<long>(std::min(LimitUm() / kStepSizeUm, 2.0e9));
   xMin = yMin = -lim;
   xMax = yMax = lim;
   return DEVICE_OK;
}
