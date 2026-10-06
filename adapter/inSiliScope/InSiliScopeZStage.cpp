///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeZStage.cpp
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   See InSiliScopeZStage.h.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "InSiliScopeZStage.h"
#include "Simulation/SharedStageState.h"

#include <algorithm>
#include <cmath>

const char* g_ZStageDeviceName = "ZStage";

InSiliScopeZStage::InSiliScopeZStage()
{
   SetRegistryErrorTexts();
}

InSiliScopeZStage::~InSiliScopeZStage()
{
   Shutdown();
}

void InSiliScopeZStage::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_ZStageDeviceName);
}

int InSiliScopeZStage::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   int ret = InitRegistry(g_ZStageDeviceName);
   if (ret != DEVICE_OK)
      return ret;
   ret = CreateStringProperty(MM::g_Keyword_Name, g_ZStageDeviceName, true);
   if (ret != DEVICE_OK)
      return ret;

   ret = CreateStringProperty(MM::g_Keyword_Description,
                               "The focus: height of the focal plane above the coverslip (um)", true);
   if (ret != DEVICE_OK)
      return ret;

   // Start 0.5 um above the coverslip (Z = 0 is the surface the cells sit
   // on, for the CellField pattern), so a fresh configuration focuses into
   // the cells. Patterns at z = 0 therefore start 0.5 um out of focus.
   Hub()->Stage().zPositionUm.store(kInitialPositionUm);

   CPropertyAction* pAct = new CPropertyAction(this, &InSiliScopeZStage::OnPosition);
   ret = CreateFloatProperty(MM::g_Keyword_Position, Hub()->Stage().zPositionUm.load(), false, pAct);
   if (ret != DEVICE_OK)
      return ret;
   SetPropertyLimits(MM::g_Keyword_Position, kLowerLimitUm, kUpperLimitUm);

   ret = UpdateStatus();
   if (ret != DEVICE_OK)
      return ret;

   initialized_ = true;
   return DEVICE_OK;
}

int InSiliScopeZStage::Shutdown()
{
   ShutdownRegistry();
   initialized_ = false;
   return DEVICE_OK;
}

int InSiliScopeZStage::SetPositionUm(double pos)
{
   pos = std::min(std::max(pos, kLowerLimitUm), kUpperLimitUm);
   Hub()->Stage().zPositionUm.store(pos);
   return OnStagePositionChanged(pos);
}

int InSiliScopeZStage::GetPositionUm(double& pos)
{
   pos = Hub()->Stage().zPositionUm.load();
   return DEVICE_OK;
}

int InSiliScopeZStage::SetPositionSteps(long steps)
{
   return SetPositionUm(steps * kStepSizeUm);
}

int InSiliScopeZStage::GetPositionSteps(long& steps)
{
   double pos;
   GetPositionUm(pos);
   steps = std::lround(pos / kStepSizeUm);
   return DEVICE_OK;
}

int InSiliScopeZStage::SetOrigin()
{
   return DEVICE_OK;
}

int InSiliScopeZStage::GetLimits(double& lower, double& upper)
{
   lower = kLowerLimitUm;
   upper = kUpperLimitUm;
   return DEVICE_OK;
}

int InSiliScopeZStage::Home()
{
   return SetPositionUm(0.0);
}

int InSiliScopeZStage::Stop()
{
   return DEVICE_OK;
}

int InSiliScopeZStage::OnPosition(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      pProp->Set(Hub()->Stage().zPositionUm.load());
   }
   else if (eAct == MM::AfterSet)
   {
      double pos;
      pProp->Get(pos);
      SetPositionUm(pos);
   }
   return DEVICE_OK;
}

int InSiliScopeZStage::StartStageSequence()
{
   Hub()->Stage().ArmZSequence(true);
   return DEVICE_OK;
}

int InSiliScopeZStage::StopStageSequence()
{
   Hub()->Stage().ArmZSequence(false);
   return OnStagePositionChanged(Hub()->Stage().zPositionUm.load());
}

int InSiliScopeZStage::ClearStageSequence()
{
   pending_.clear();
   return DEVICE_OK;
}

int InSiliScopeZStage::AddToStageSequence(double position)
{
   if (static_cast<long>(pending_.size()) >= kMaxSequence)
      return DEVICE_SEQUENCE_TOO_LARGE;
   pending_.push_back(std::min(std::max(position, kLowerLimitUm), kUpperLimitUm));
   return DEVICE_OK;
}

int InSiliScopeZStage::SendStageSequence()
{
   Hub()->Stage().SetZSequence(pending_);
   return DEVICE_OK;
}

int InSiliScopeZStage::SetStageLinearSequence(double dZ_um, long nSlices)
{
   if (nSlices < 1 || nSlices > kMaxSequence)
      return DEVICE_SEQUENCE_TOO_LARGE;
   // Steps of dZ from the current position; the N-th trigger returns to it.
   const double z0 = Hub()->Stage().zPositionUm.load();
   std::vector<double> seq;
   for (long i = 0; i < nSlices; ++i)
      seq.push_back(std::min(std::max(z0 + i * dZ_um, kLowerLimitUm), kUpperLimitUm));
   Hub()->Stage().SetZSequence(seq);
   return DEVICE_OK;
}
