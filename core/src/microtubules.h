// Per-cell 3D microtubule centrelines. Port of the JS prototype's
// microtubules.js (buildMicrotubulesForCell and everything it calls); see
// spec/ALGORITHM.md for the why of each pass. Cell-local frame (centre-
// relative, before packRot), um.
//
// Draw order matters: every mtGenerateOne draw comes from one hashStream in
// JS statement order, so the port draws into named locals one at a time.
#pragma once

#include "cells.h"
#include "cytomesh.h"
#include "params.h"

#include <cstdint>
#include <vector>

namespace isc {

constexpr uint32_t MT_CH_COUNT = 500;
constexpr uint32_t MT_STREAM_BASE = 1000;
constexpr uint32_t MT_RESAMPLE_SPACING = 100000;
// Start/end sampling: at most MT_SAMPLE_TRIES candidates per point; the start
// region reaches MT_DECAY_SPAN decay lengths; decay lengths are floored at
// MT_MIN_DECAY_UM; an end is picked from MT_END_CANDIDATES by its direction.
constexpr int MT_SAMPLE_TRIES = 128;
constexpr double MT_DECAY_SPAN = 5;
constexpr double MT_MIN_DECAY_UM = 0.02;
constexpr int MT_END_CANDIDATES = 12;
constexpr int MT_RMAX_SAMPLES = 512;
constexpr int MT_NUDGE_ROUNDS = 5;
constexpr int MT_RESAMPLE_ROUNDS = 2;
constexpr int MT_POST_RESAMPLE_NUDGE_ROUNDS = 3;
constexpr long MT_COLLISION_OP_BUDGET = 300000;
constexpr size_t MT_COLLISION_MAX_TOTAL_POINTS = 8000;   // collision pass skipped above this (documented)
constexpr int MT_MAX_STEPS_PER_MT = 1500;
constexpr int MT_MAX_PER_CELL = 5000;
constexpr double MT_CONTAIN_MARGIN = 0.98;
// Obstacle envelope (mtObstacleEnvelope): ramp slope (z per um in xy) and the
// xy arc over which an obstacle's clearance fades in from the start.
constexpr double MT_OBST_RAMP_SLOPE = 1;
constexpr double MT_OBST_CLEAR_RAMP_UM = 1;
constexpr int MT_MAX_END_TRIM = 50;

struct Microtubule {
   std::vector<Pt3> pts;
   double priority = 0;
};

// Shape-only per-cell data the generator needs (JS getMtCellGeometry +
// the smoothed cytoplasm mesh it clamps against).
struct MtCellGeom {
   double areaUm2 = 0;
   double sizeUm = 0;          // equivalent diameter 2 sqrt(area / pi)
   double nucBox[4] = {};      // nucleus footprint bounding box x0, y0, x1, y1
   double rMax = 0;            // largest outline radius x MT_CONTAIN_MARGIN
   CytoMesh mesh;
   // cos/sin(-nucRot) and cos/sin(nucRot), as the per-point nucleus tests
   // compute them (once per cell here instead of once per point).
   double nucCosNeg = 1, nucSinNeg = 0, nucCosPos = 1, nucSinPos = 0;
};

MtCellGeom BuildMtCellGeom(const Cell& c, const Params& p);
int MtCountForCell(uint32_t seed, const Cell& c, const Params& p, const MtCellGeom& g);
Microtubule MtGenerateOne(uint32_t seed, int mtIndex, int resampleRound, const Cell& c, const Params& p, const MtCellGeom& g);

// All microtubules of one cell, collision-resolved and post-processed. `g`
// may be passed in when the caller already built it.
std::vector<Microtubule> BuildMicrotubulesForCell(uint32_t seed, const Cell& c, const Params& p, const MtCellGeom& g);

// Per-segment arc length and parallel-transport frame (T along the axis,
// U/V spanning the cross-section), exactly as buildMicrotubuleLabelPoints
// builds it. Segment i runs pts[i] -> pts[i+1]; cum has pts.size() entries.
struct MtFrames {
   std::vector<double> cum;
   std::vector<Pt3> T, U, V;
   double Length() const { return cum.empty() ? 0 : cum.back(); }
};
MtFrames BuildMtFrames(const std::vector<Pt3>& pts);

} // namespace isc
