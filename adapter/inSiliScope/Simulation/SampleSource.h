///////////////////////////////////////////////////////////////////////////////
// FILE:          SampleSource.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The mounted sample's event source (spec/PORT.md 6.2, 8
//                and 20; was CellFieldSource until the nanorulers, ABI 12):
//                blinks of the insiliscope world's dyes -- the cell field's
//                or the DNA-origami nanorulers', by the core param
//                'specimen' -- fetched through the core's C ABI
//                (core/include/insiliscope/insiliscope.h) and translated
//                into ordinary BlinkEvents in a FOV's frame, so
//                RenderPhotonImage / CollectGpuEmitters / ApplyNoiseChain
//                render them unchanged.
//
//                A dye's blinks are a pure function
//                of its address in the world (seed, cell, microtubule,
//                lattice site; tile, molecule, strand), not draws per blink,
//                so the same dye blinks the same way whenever the FOV
//                returns to it. Not thread-safe (the core world is not): one
//                instance per thread.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "SMLMSimulation.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

struct IscWorld;

namespace sim {

struct SampleSettings
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
   bool SameWorld(const SampleSettings& o) const { return seed == o.seed && params == o.params; }
   // Bit for bit: a label's linkage slots may be NaN (the structure's built-in
   // linkage), and NaN == NaN is false (every live frame would reset the labels).
   bool SameLabels(const SampleSettings& o) const
   {
      if (labels.size() != o.labels.size())
         return false;
      for (size_t i = 0; i < labels.size(); ++i)
         if (labels[i].size() != o.labels[i].size() ||
             (!labels[i].empty() && std::memcmp(labels[i].data(), o.labels[i].data(), labels[i].size() * sizeof(double)) != 0))
            return false;
      return true;
   }
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
struct SampleQuery
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

class SampleSource
{
public:
   SampleSource() = default;
   ~SampleSource();
   SampleSource(const SampleSource&) = delete;
   SampleSource& operator=(const SampleSource&) = delete;

   // (Re)creates the world when the seed or params change (dropping every
   // cache), and only updates the labels when just those change. False
   // (with err) on an unknown param, an invalid label or a core failure.
   bool Configure(const SampleSettings& s, std::string& err);
   bool Ready() const { return world_ != nullptr; }
   // The configured core world (nullptr before Configure), for geometry
   // queries through the C ABI (ScopeGeometryJson).
   IscWorld* World() const { return world_; }

   // Appends the blinks of q to out. False on a core failure.
   bool Events(const SampleQuery& q, std::vector<BlinkEvent>& out);
   // Appends the continuous windows (isc_continuous_in_window: PALM pre
   // states, dSTORM initial ON, WideField dyes) open after q.tSec, mapped as
   // Events maps blinks (an always-on window ends at +infinity).
   bool Continuous(const SampleQuery& q, std::vector<BlinkEvent>& out);
   // The core's rows of isc_continuous_in_window over a box (ISC_EVENT_STRIDE doubles each: world x, y, z, clock
   // seconds), replacing rows: a host's cache of windows (each a pure function of its dye and the kinetics history),
   // mapped into a query's frame by MapRow.
   bool ContinuousRows(double x0, double y0, double x1, double y1, double zMin, double zMax, double tMin,
                       std::vector<double>& rows);
   // One row (ISC_EVENT_STRIDE doubles) as a BlinkEvent in q's frame, as Events / Continuous map them.
   static void MapRow(const SampleQuery& q, const double* row, BlinkEvent& out);
   // The fluorescent dyes in the window (isc_sites_in_window): ISC_SITE_STRIDE doubles each (world x, y, z, id,
   // structure), replacing out. False on a core failure.
   bool Sites(double x0, double y0, double x1, double y1, double zMin, double zMax, std::vector<double>& out);

   // Pre-loads what q would need if the stage moved up to marginUm in x/y or
   // to any focus (the whole z column), nearest ring first, for at most about
   // budgetMs. Only fills caches: no event changes. True when all of it is
   // cached.
   bool Prefetch(const SampleQuery& q, double marginUm, double budgetMs);

   // The kinetics history of the following Events / Continuous / Prefetch
   // (isc_world_set_kinetics_history, ABI 11: nSeg rows of ISC_KIN_ROW
   // doubles; 0 = none, each label's kinetics from t = 0). A host that sets
   // one sets it before every query (the world is shared). False on bad rows.
   bool SetKineticsHistory(const double* rows, int nSeg);

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
   // a nanoruler world: the highest a dye can sit when a molecule reaches it
   // (isc_origami_heights; JS World.maxCellHeight); 0 if none, -1 on a
   // failure.
   double MaxCellHeight(double x0, double y0, double x1, double y1);

private:
   IscWorld* world_ = nullptr;
   SampleSettings settings_;
   bool cacheDirApplied_ = false;   // settings_.cacheDir given to world_ (a new world starts without)
   double origamiTopUm_ = -1;        // a nanoruler world's isc_origami_heights out[1]; < 0 in a cell field
   std::vector<double> buf_;
};

} // namespace sim
