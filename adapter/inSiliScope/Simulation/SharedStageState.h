///////////////////////////////////////////////////////////////////////////////
// FILE:          SharedStageState.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The stage state shared by the inSiliScope devices: the ZStage
//                writes zPositionUm (and its hardware z sequence), the XYStage
//                drives the XY motion model, and the camera reads both for
//                every frame it renders (one pose per frame, spec/PORT.md 7).
//                One instance, owned by the hub (InSiliScopeHub::Stage()); no
//                device knows another.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace sim {

struct SharedStageState
{
   using Clock = std::chrono::steady_clock;

   std::atomic<double> zPositionUm{0.0};

   // ---- Z sequence (MM hardware-triggered z stacks) ----
   // The ZStage uploads positions (AddToStageSequence / SetStageLinear-
   // Sequence, then SendStageSequence) and arms them (StartStageSequence).
   // While armed, the camera takes one position per frame of a sequence
   // acquisition, in order, cycling -- the trigger a real camera would send
   // the stage. version changes whenever the sent sequence does.
   struct ZSequence
   {
      std::vector<double> positions;
      bool armed = false;
      long version = 0;
   };

   void SetZSequence(const std::vector<double>& positions)
   {
      std::lock_guard<std::mutex> g(zSeqMutex_);
      zSeq_.positions = positions;
      zSeq_.version++;
      zSeqIndex_ = 0;
   }
   // Armed: the next frame takes position 0. Disarming returns the stage to
   // where it was when armed (MM's linear-sequence contract, kept for both).
   void ArmZSequence(bool armed)
   {
      std::lock_guard<std::mutex> g(zSeqMutex_);
      if (armed && !zSeq_.armed)
         zBeforeSeq_ = zPositionUm.load();
      if (!armed && zSeq_.armed)
         zPositionUm.store(zBeforeSeq_);
      zSeq_.armed = armed && !zSeq_.positions.empty();
      zSeqIndex_ = 0;
   }
   ZSequence GetZSequence()
   {
      std::lock_guard<std::mutex> g(zSeqMutex_);
      return zSeq_;
   }
   // A camera sequence acquisition starts: an armed sequence restarts at
   // position 0. Returns the new acquisition epoch (frames rendered before it
   // carry an older one).
   long BeginSequenceAcquisition()
   {
      std::lock_guard<std::mutex> g(zSeqMutex_);
      zSeqIndex_ = 0;
      capturing_ = true;
      return ++epoch_;
   }
   void EndSequenceAcquisition()
   {
      std::lock_guard<std::mutex> g(zSeqMutex_);
      capturing_ = false;
   }
   // The focus for the next frame: during a sequence acquisition with an
   // armed sequence, its next position (the stage moves there), else the
   // current one. epoch: the acquisition epoch the frame belongs to.
   double NextFrameZ(long* epoch = nullptr)
   {
      std::lock_guard<std::mutex> g(zSeqMutex_);
      if (epoch)
         *epoch = epoch_;
      if (!capturing_ || !zSeq_.armed || zSeq_.positions.empty())
         return zPositionUm.load();
      const double z = zSeq_.positions[zSeqIndex_ % zSeq_.positions.size()];
      ++zSeqIndex_;
      zPositionUm.store(z);
      return z;
   }

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

   // The target of the current (or last) move.
   void XyTarget(double& x, double& y)
   {
      std::lock_guard<std::mutex> g(xyMutex_);
      x = toX_;
      y = toY_;
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

   std::mutex zSeqMutex_;
   ZSequence zSeq_;
   size_t zSeqIndex_ = 0;
   double zBeforeSeq_ = 0.0;
   bool capturing_ = false;
   long epoch_ = 0;

   std::mutex xyMutex_;
   double fromX_ = 0.0, fromY_ = 0.0, toX_ = 0.0, toY_ = 0.0;
   Clock::time_point moveStart_ = Clock::now() - std::chrono::hours(1);
   double speedUmPerSec_ = 5000.0;
   double settleSec_ = 0.02;
};

} // namespace sim
