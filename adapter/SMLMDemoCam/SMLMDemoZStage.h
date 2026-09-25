///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMDemoZStage.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   A single-axis Z-stage device providing a global focus offset
//                for the SMLMDemoCam camera's vectorial PSF renderer. Add
//                both "SMLMDemoCam" and "SMLMDemoZStage" via the Hardware
//                Configuration Wizard; no explicit linking between the two
//                devices is needed -- they communicate through the
//                process-wide Simulation/SharedStageState.h singleton, the
//                same way MM itself treats camera and focus stage as
//                independent devices.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "DeviceBase.h"

extern const char* g_SMLMZStageDeviceName;

class SMLMDemoZStage : public CStageBase<SMLMDemoZStage>
{
public:
   SMLMDemoZStage();
   ~SMLMDemoZStage();

   // MMDevice API
   int Initialize();
   int Shutdown();
   void GetName(char* name) const;
   bool Busy() { return false; }

   // Stage API
   int SetPositionUm(double pos);
   int GetPositionUm(double& pos);
   int SetPositionSteps(long steps);
   int GetPositionSteps(long& steps);
   int SetOrigin();
   int GetLimits(double& lower, double& upper);
   int Home();
   int Stop();
   bool IsContinuousFocusDrive() const { return false; }
   // Not sequenceable -- position changes come from the user/UI (or a
   // script), not TTL-triggered sequences.
   int IsStageSequenceable(bool& isSequenceable) const
   {
      isSequenceable = false;
      return DEVICE_OK;
   }

   // action interface
   int OnPosition(MM::PropertyBase* pProp, MM::ActionType eAct);

private:
   static constexpr double kStepSizeUm = 0.001;
   static constexpr double kLowerLimitUm = -50.0;
   static constexpr double kUpperLimitUm = 50.0;

   bool initialized_ = false;
};
