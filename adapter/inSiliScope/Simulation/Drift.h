///////////////////////////////////////////////////////////////////////////////
// FILE:          Drift.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Sample drift, xy and z set separately: a directed part
//                (a mean velocity whose xy direction and xy/z strength
//                wander slowly, below) plus a random walk. Random walk: every
//                frame adds an independent normal step per axis, summed
//                ("cumulative normal": Cnossen et al., Opt. Express 29,
//                27961 (2021); the 5/10/20 nm/s RMS drifts of Ma et al.,
//                Sci. Adv. 10, eadm7765 (2024)). The step's variance is
//                sigma^2 x frame time (Brownian scaling), so sigma is the
//                RMS displacement per axis after 1 s whatever the frame rate
//                (strictly nm / sqrt(s)). x and y each get sigma_xy.
//
//                Directed part (2026-10-06): velocity v_xy = V_xy max(0, 1 +
//                s_xy) along theta0 + phi, v_z = V_z max(0, 1 + s_z) (V_z
//                signed). theta0 is set or drawn once per seed; phi, s_xy, s_z
//                are Ornstein-Uhlenbeck processes (mean 0, RMS angleWander /
//                speedWander, correlation time wanderTimeSec), started from
//                their stationary distribution, updated exactly per frame
//                (x' = a x + sigma sqrt(1 - a^2) g, a = exp(-dt / tau)). A frame
//                moves the sample by v dt (v at the frame's start) plus the
//                random-walk step. Draws: random walk on pixel 0 of
//                CounterRng(seed, f) (unchanged), the wanders on pixel 1,
//                theta0 on pixel 2 of frame 0xFFFFFFFF.
//
//                The drift is the sample's displacement in camera axes (+z
//                away from the coverslip), zero at an acquisition's first
//                frame and constant within a frame. Every step is a pure
//                function of (seed, frame) (counter-based draws), so a stack
//                and live mode follow one path for one seed. The JS twin is
//                web/prototype/scope/drift.js.
//
//                Used by: SR (emitter offset, focus - dz), WideField and
//                BrightField (DriftFrames: the full-grid image spectrum
//                shifted by a phase ramp per frame, z by images on a focus
//                grid, interpolated).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Fft2d.h"

#include <cstdint>
#include <vector>

namespace sim {

struct DriftSettings
{
   // Random walk: RMS displacement after 1 s, x and y each / z.
   double xyNmPerSqrtSec = 0.0;
   double zNmPerSqrtSec = 0.0;
   // Directed: mean speeds (z signed), the xy direction (< 0: random per
   // seed), and the slow wander of direction and strength.
   double xySpeedNmPerSec = 0.0;
   double zSpeedNmPerSec = 0.0;
   double xyAngleDeg = -1.0;
   double angleWanderDeg = 0.0;  // RMS of the direction about its mean
   double speedWanderPct = 0.0;  // RMS of the xy and z strengths (each), % of the mean
   double wanderTimeSec = 60.0;  // correlation time of the wanders
   bool On() const
   {
      return xyNmPerSqrtSec > 0.0 || zNmPerSqrtSec > 0.0 || xySpeedNmPerSec != 0.0 || zSpeedNmPerSec != 0.0;
   }
};

struct DriftNm
{
   double x = 0.0, y = 0.0, z = 0.0;
};

// The drift stream's seed for a RandomSeed (its own stream: no other draw moves).
uint32_t DriftSeed(long randomSeed);
// The random-walk step from frame f - 1 to frame f (f >= 1), nm.
DriftNm DriftStep(uint32_t driftSeed, long f, double frameSec, const DriftSettings& s);

// The drift frame by frame: zero at frame f0, then Step() per frame (each
// with its own frame time; the settings may change between steps, the
// wander state carries on). Stacks start at 0; live mode at the frame the
// drift was switched on or the acquisition started.
class DriftWalker
{
public:
   DriftWalker() = default;
   DriftWalker(long randomSeed, const DriftSettings& s, long f0 = 0);
   void SetSettings(const DriftSettings& s) { s_ = s; }
   const DriftNm& Step(double frameSec);
   const DriftNm& Position() const { return d_; }
   long Frame() const { return f_; }

private:
   uint32_t seed_ = 0;
   DriftSettings s_;
   long f_ = 0;
   DriftNm d_;
   double theta0_ = 0.0, phi_ = 0.0, sxy_ = 0.0, sz_ = 0.0; // phi in units of the angle wander, s in units of the speed wander
};

// d(0) = 0, then one DriftWalker step per frame, for f = 0 .. frames - 1.
std::vector<DriftNm> DriftTrajectory(long randomSeed, long frames, double frameSec, const DriftSettings& s);

struct DriftBounds
{
   double xLo = 0, xHi = 0, yLo = 0, yHi = 0, zLo = 0, zHi = 0; // nm
   double MaxXyNm() const;
};
DriftBounds DriftRange(const std::vector<DriftNm>& t);
// A z cull window (centre +/- half, um) around a focal plane that sits dz
// lower in the sample: grown to hold every frame's window.
void DriftWidenZCull(const DriftBounds& b, double& centreUm, double& halfUm);

// z drift for the WideField/BrightField images: foci on a grid of
// kDriftFocusStepNm over the trajectory's z range; a frame's image is the
// linear interpolation of its two neighbours (relative error ~ step^2/8 x
// d2I/dz2 / I; cli/drift_check checks < 1e-3).
constexpr double kDriftFocusStepNm = 10.0;
struct DriftFocusGrid
{
   long k0 = 0; // grid index of the lowest focus: dz = (k0 + k) x step
   int n = 1;   // foci
   static DriftFocusGrid For(const DriftBounds& b);
   double DzNm(int k) const { return (k0 + k) * kDriftFocusStepNm; }
   // image(dz) = (1 - w) image(k) + w image(k + 1) (k + 1 < n, or w = 0).
   void Weights(double dzNm, int& k, double& w) const;
};

// Multiplies a RealFft2d spectrum (ny rows of nx / 2 + 1) by exp(+2 pi i k
// frac / n) per axis: the inverse is then image(j + frac) (Nyquist bins: the
// cosine, so the image stays real). WideField's sub-cell pose shift and the
// drift shift of WideField/BrightField frames.
void ApplyShiftRamp(cfloat* S, unsigned nx, unsigned ny, double fracX, double fracY);

// The camera frame of a periodic, band-limited full-grid image given by its
// spectrum: shifted so that out(j) = image(j + frac) (cells), cells [x0, x0 +
// cw) x [y0, y0 + ch) kept, each max(0, .) and summed (scale x the sum) over
// up x up cells into cam (+=; cw / up x ch / up). spec is destroyed.
void ShiftedCamera(const RealFft2d& fft, std::vector<cfloat>& spec, double fracX, double fracY, unsigned x0,
                   unsigned y0, unsigned cw, unsigned ch, unsigned up, float scale, std::vector<float>& cam);

} // namespace sim
