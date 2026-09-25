///////////////////////////////////////////////////////////////////////////////
// FILE:          SharedStageState.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Process-wide shared state linking the InSiliCellScopeZStage device
//                (InSiliCellScopeZStage.h/.cpp) to CInSiliCellScopeCamera's frame renderer,
//                without either device needing to know about the other or
//                MM's device-linking mechanism: InSiliCellScopeZStage writes
//                zPositionUm, and both StackGenerationWorker and
//                LiveProducerLoop (SMLMImageGeneration.cpp) read it each
//                frame as a uniform focus offset applied to every emitter
//                (see RenderPhotonImage's globalZOffsetUm parameter in
//                SMLMSimulation.h). InSiliCellScopeXYStage (InSiliCellScopeXYStage.h/.cpp)
//                likewise drives the XY motion model below, which the camera
//                samples once per produced frame for the CellField pattern
//                (spec/PORT.md section 7). A single process-wide instance is
//                sufficient -- Micro-Manager loads one instance of each
//                device type per process.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace sim {

struct SharedStageState
{
   using Clock = std::chrono::steady_clock;

   std::atomic<double> zPositionUm{0.0};

   // ---- XY: world coordinate of the FOV centre, um (spec/PORT.md 7.3) ----
   // Constant-velocity move from the position at the time of the request to
   // the target, then settleSec of "busy" after arrival. Motion blur during
   // an exposure is ignored on purpose: the camera samples one pose per frame.

   // Starts a move to (x, y) from the current interpolated position.
   void SetXyTarget(double x, double y, Clock::time_point now = Clock::now())
   {
      std::lock_guard<std::mutex> g(xyMutex_);
      double cx, cy;
      PositionLocked(now, cx, cy);
      fromX_ = cx; fromY_ = cy;
      toX_ = x; toY_ = y;
      moveStart_ = now;
   }

   // Interpolated position at `now`.
   void PositionXyAt(Clock::time_point now, double& x, double& y)
   {
      std::lock_guard<std::mutex> g(xyMutex_);
      PositionLocked(now, x, y);
   }

   // True until arrival + settle time.
   bool XyBusy(Clock::time_point now = Clock::now())
   {
      std::lock_guard<std::mutex> g(xyMutex_);
      return Seconds(now - moveStart_) < MoveSecLocked() + settleSec_;
   }

   // Ends a move where it currently is.
   void StopXy(Clock::time_point now = Clock::now())
   {
      std::lock_guard<std::mutex> g(xyMutex_);
      double cx, cy;
      PositionLocked(now, cx, cy);
      fromX_ = toX_ = cx; fromY_ = toY_ = cy;
      moveStart_ = now - std::chrono::hours(1);
   }

   void SetXySpeed(double umPerSec) { std::lock_guard<std::mutex> g(xyMutex_); speedUmPerSec_ = std::max(1e-3, umPerSec); }
   void SetXySettleSec(double s) { std::lock_guard<std::mutex> g(xyMutex_); settleSec_ = std::max(0.0, s); }
   double XySpeed() { std::lock_guard<std::mutex> g(xyMutex_); return speedUmPerSec_; }
   double XySettleSec() { std::lock_guard<std::mutex> g(xyMutex_); return settleSec_; }

private:
   static double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
   double MoveSecLocked() const { return std::hypot(toX_ - fromX_, toY_ - fromY_) / speedUmPerSec_; }
   void PositionLocked(Clock::time_point now, double& x, double& y) const
   {
      const double T = MoveSecLocked(), t = Seconds(now - moveStart_);
      const double f = T > 0 ? std::min(1.0, std::max(0.0, t / T)) : 1.0;
      x = fromX_ + (toX_ - fromX_) * f;
      y = fromY_ + (toY_ - fromY_) * f;
   }

   std::mutex xyMutex_;
   double fromX_ = 0.0, fromY_ = 0.0, toX_ = 0.0, toY_ = 0.0;
   Clock::time_point moveStart_ = Clock::now() - std::chrono::hours(1);
   double speedUmPerSec_ = 5000.0;
   double settleSec_ = 0.02;
};

// Process-wide singleton, lazily constructed on first use (thread-safe by
// C++11 function-local static initialization rules).
inline SharedStageState& GetSharedStageState()
{
   static SharedStageState state;
   return state;
}

} // namespace sim
