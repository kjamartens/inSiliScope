///////////////////////////////////////////////////////////////////////////////
// FILE:          SharedStageState.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Process-wide shared state linking the SMLMDemoZStage device
//                (SMLMDemoZStage.h/.cpp) to CSMLMDemoCamera's frame renderer,
//                without either device needing to know about the other or
//                MM's device-linking mechanism: SMLMDemoZStage writes
//                zPositionUm, and both StackGenerationWorker and
//                LiveProducerLoop (SMLMImageGeneration.cpp) read it each
//                frame as a uniform focus offset applied to every emitter
//                (see RenderPhotonImage's globalZOffsetUm parameter in
//                SMLMSimulation.h). A single process-wide instance is
//                sufficient -- Micro-Manager loads one instance of each
//                device type per process.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <atomic>

namespace sim {

struct SharedStageState
{
   std::atomic<double> zPositionUm{0.0};
};

// Process-wide singleton, lazily constructed on first use (thread-safe by
// C++11 function-local static initialization rules).
inline SharedStageState& GetSharedStageState()
{
   static SharedStageState state;
   return state;
}

} // namespace sim
