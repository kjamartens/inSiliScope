///////////////////////////////////////////////////////////////////////////////
// FILE:          InSiliScopeXYStage.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   A simulated XY stage moving inSiliScope's field of view over
//                the CellField pattern's infinite cell field (spec/PORT.md
//                section 7). Its position is the world coordinate of the FOV
//                centre in um; a move runs at General_StageSpeedUmPerSec and
//                reports Busy until it arrives plus General_StageSettleMs.
//                Add both "inSiliScope" and "InSiliScopeXYStage" via the Hardware
//                Configuration Wizard; like InSiliScopeZStage, they communicate
//                through the process-wide Simulation/SharedStageState.h
//                singleton. Direction conventions (camera mirroring) are MM's
//                standard TransposeMirrorX/Y properties of every XY stage.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "DeviceBase.h"
#include "Registry/RegistryDevice.h"

extern const char* g_XYStageDeviceName;

class InSiliScopeXYStage : public isc::RegistryDevice<InSiliScopeXYStage, CXYStageBase<InSiliScopeXYStage>>
{
public:
   InSiliScopeXYStage();
   ~InSiliScopeXYStage();

   // MMDevice API
   int Initialize();
   int Shutdown();
   void GetName(char* name) const;
   bool Busy();

   // XYStage API. The base class maps um <-> steps (adapter origin,
   // mirroring); steps here are hardware positions of kStepSizeUm.
   int SetPositionSteps(long x, long y);
   int GetPositionSteps(long& x, long& y);
   int SetRelativePositionSteps(long x, long y);
   int Home();
   int Stop();
   int SetOrigin();
   int GetLimitsUm(double& xMin, double& xMax, double& yMin, double& yMax);
   int GetStepLimits(long& xMin, long& xMax, long& yMin, long& yMax);
   double GetStepSizeXUm() { return kStepSizeUm; }
   double GetStepSizeYUm() { return kStepSizeUm; }
   int IsXYStageSequenceable(bool& isSequenceable) const
   {
      isSequenceable = false;
      return DEVICE_OK;
   }

private:
   static constexpr double kStepSizeUm = 0.01;

   double LimitUm() const { return Hub() ? Hub()->State().xyLimitUm.load() : 1e6; }

   bool initialized_ = false;
};
