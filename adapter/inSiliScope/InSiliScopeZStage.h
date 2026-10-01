///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeZStage.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   A single-axis Z-stage device providing a global focus offset
//                for the inSiliScope camera's vectorial PSF renderer. Add
//                both "inSiliScope" and "InSiliScopeZStage" via the Hardware
//                Configuration Wizard; no explicit linking between the two
//                devices is needed -- they communicate through the
//                process-wide Simulation/SharedStageState.h singleton, the
//                same way MM itself treats camera and focus stage as
//                independent devices.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <vector>

#include "DeviceBase.h"

extern const char* g_ZStageDeviceName;

class InSiliScopeZStage : public CStageBase<InSiliScopeZStage>
{
public:
   InSiliScopeZStage();
   ~InSiliScopeZStage();

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
   // Sequenceable: MM uploads a z stack and the camera steps through it,
   // one position per frame of a sequence acquisition (the TTL a real
   // camera would send), via SharedStageState.
   int IsStageSequenceable(bool& isSequenceable) const
   {
      isSequenceable = true;
      return DEVICE_OK;
   }
   int IsStageLinearSequenceable(bool& isSequenceable) const
   {
      isSequenceable = true;
      return DEVICE_OK;
   }
   int GetStageSequenceMaxLength(long& nrEvents) const
   {
      nrEvents = kMaxSequence;
      return DEVICE_OK;
   }
   int StartStageSequence();
   int StopStageSequence();
   int ClearStageSequence();
   int AddToStageSequence(double position);
   int SendStageSequence();
   int SetStageLinearSequence(double dZ_um, long nSlices);

   // action interface
   int OnPosition(MM::PropertyBase* pProp, MM::ActionType eAct);

private:
   static constexpr double kStepSizeUm = 0.001;
   static constexpr double kInitialPositionUm = 0.5;   // set on Initialize()
   static constexpr double kLowerLimitUm = -50.0;
   static constexpr double kUpperLimitUm = 50.0;
   static constexpr long kMaxSequence = 100000;

   bool initialized_ = false;
   std::vector<double> pending_; // AddToStageSequence, until SendStageSequence
};
