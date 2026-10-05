///////////////////////////////////////////////////////////////////////////////
// FILE:          CellFieldSource.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The CellField pattern's event source (spec/PORT.md 6.2 and
//                8): blinks of the insiliscope world's dyes, fetched through
//                the core's C ABI (core/include/insiliscope/insiliscope.h) and
//                translated into ordinary BlinkEvents in a FOV's frame, so
//                RenderPhotonImage / CollectGpuEmitters / ApplyNoiseChain
//                render them unchanged.
//
//                A dye's blinks are a pure function
//                of its address in the world (seed, cell, microtubule,
//                lattice site), not draws per blink, so the same dye blinks
//                the same way whenever the FOV returns to it. Not thread-
//                safe (the core world is not): one instance per thread.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "SMLMSimulation.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct IscWorld;

namespace sim {

struct CellFieldSettings
{
   uint32_t seed = 0;                                   // world seed (derive it from RandomSeed)
   std::vector<std::pair<std::string, double>> params;  // core param names (the prototype's), e.g. mtDensity
   // One label per structure (index = ISC_STRUCT_*), each in the
   // isc_world_set_label layout (ISC_LABEL_COUNT doubles; fewer: the rest
   // default); no entry = the core's default label (ABI 10).
   std::vector<std::vector<double>> labels;

   // Directory of the core's packed-block store (isc_world_set_cache_dir, ABI
   // 8; CacheDir.h names the per-user default): "" = none. Not part of
   // SameWorld: switching it keeps the world.
   std::string cacheDir;
   bool SameWorld(const CellFieldSettings& o) const { return seed == o.seed && params == o.params; }
   bool SameLabels(const CellFieldSettings& o) const { return labels == o.labels; }
   // The label mode of structure s (ISC_MODE_*; the core default DNA-PAINT
   // when not given).
   int LabelMode(int s) const;
};

// A label in the isc_world_set_label layout (ISC_LABEL_COUNT doubles).
std::vector<double> MakeLabelVector(int mode, double density, double fluorescentFraction, double activationRatePerSec,
                                    double onSec, double offSec, double bleachProb, double photonCV,
                                    double initialOnSec = 0.0, bool preState = false);

// Where and when to look. World um (z = height above the coverslip) and
// simulated seconds.
struct CellFieldQuery
{
   // World coordinate of the FOV's top-left corner: BlinkEvent x/y are
   // relative to it (the renderer then adds drift).
   double originXUm = 0.0, originYUm = 0.0;
   // World rect to take dyes from (the FOV, shifted against drift, plus a
   // PSF margin).
   double x0Um = 0.0, y0Um = 0.0, x1Um = 0.0, y1Um = 0.0;
   // BlinkEvent zNm = (z - zRefUm) * 1000 (the renderer then adds the Z
   // stage offset). Dyes with |z - zCullCentreUm| > zHalfRangeUm are culled,
   // not clamped: the renderer would otherwise draw them on the kernel's end
   // plane (<= 0: no z limit).
   double zRefUm = 0.0;
   double zCullCentreUm = 0.0;
   double zHalfRangeUm = 0.0;
   // Blinks overlapping [tSec, tSec + spanSec); tStart/tEnd come out in
   // frames with frame `frameIndex` starting at tSec:
   // tStart = frameIndex + (tOn - tSec) / frameSec.
   double tSec = 0.0, spanSec = 0.0, frameSec = 0.05;
   long frameIndex = 0;
};

class CellFieldSource
{
public:
   CellFieldSource() = default;
   ~CellFieldSource();
   CellFieldSource(const CellFieldSource&) = delete;
   CellFieldSource& operator=(const CellFieldSource&) = delete;

   // (Re)creates the world when the seed or params change (dropping every
   // cache), and only updates the labels when just those change. False
   // (with err) on an unknown param, an invalid label or a core failure.
   bool Configure(const CellFieldSettings& s, std::string& err);
   bool Ready() const { return world_ != nullptr; }
   // The configured core world (nullptr before Configure), for geometry
   // queries through the C ABI (ScopeGeometryJson).
   IscWorld* World() const { return world_; }

   // Appends the blinks of q to out. False on a core failure.
   bool Events(const CellFieldQuery& q, std::vector<BlinkEvent>& out);

   // Pre-loads what q would need if the stage moved up to marginUm in x/y or
   // to any focus (the whole z column), nearest ring first, for at most about
   // budgetMs. Only fills caches: no event changes. True when all of it is
   // cached.
   bool Prefetch(const CellFieldQuery& q, double marginUm, double budgetMs);

   // Fluorescent-dye counts on an nx x ny x nz grid over [x0,x1) x [y0,y1) x
   // [zMin,zMax) (world um), out[(k*ny + iy)*nx + ix], of the structures in
   // structureMask (bit s = ISC_STRUCT_s; isc_density3d_in_window). Returns
   // the total, or -1 on a failure.
   long Density3d(double x0, double y0, double x1, double y1, double zMin, double zMax, int nx, int ny, int nz,
                  int structureMask, float* out);
   // Every structure's bit.
   static int AllStructures();
   // The structures whose dyes never bleach in a WideField image (DNA-PAINT
   // labels) and the others.
   int PersistentMask() const;
   int BleachingMask() const { return AllStructures() & ~PersistentMask(); }

   // Volume fractions of cytoplasm, nucleus and microtubules per voxel
   // (isc_optical_volume_in_window, channel-major out[3*nx*ny*nz]; zMin/zMax
   // finite, sub x sub samples per voxel column). Returns the cell count, or
   // -1 on a failure.
   long OpticalVolume(double x0, double y0, double x1, double y1, double zMin, double zMax, int nx, int ny, int nz,
                      int sub, float* out);
   // Tallest cell (peak height, um) whose footprint circle reaches the rect;
   // 0 if none, -1 on a failure.
   double MaxCellHeight(double x0, double y0, double x1, double y1);

private:
   IscWorld* world_ = nullptr;
   CellFieldSettings settings_;
   bool cacheDirApplied_ = false;   // settings_.cacheDir given to world_ (a new world starts without)
   std::vector<double> buf_;
};

} // namespace sim
