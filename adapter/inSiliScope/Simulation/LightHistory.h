///////////////////////////////////////////////////////////////////////////////
// FILE:          LightHistory.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The sample's light history, exact (2026-10-15, spec/ALGORITHM.md
//                "Exact light history"): what light every place of the sample
//                has had, as the clock map of core ABI 14 reads it
//                (insiliscope.h isc_world_set_clock_map): each dye's kinetics
//                at the dose at its own (x, y, z), smooth in all three (no
//                levels, regions or z bands: it replaced IlluminationHistory,
//                the stepped history, removed 2026-10-16).
//
//                Time. The sample's time T (s) advances by a frame's exposure
//                whenever the sample's time runs (a lit frame, or a dark one
//                while it runs idle); the history is a list of records, each an
//                interval of T.
//                Record: a maximal interval of one light -- its chapter, its
//                magnitude (total power at the sample over the chapter's
//                reference) and its pose (the optical axis within
//                kLightPoseTolUm of its first frame's, which it keeps; a Flat
//                beam: the same field stop on the grid; a drifting movie's
//                record holds its frames' poses, weighted) -- or of no light.
//                Chapter: a maximal interval of one kind of light -- the
//                geometry but the HILO pivot (profile, BFP position, objective,
//                medium), the kinetic env but the laser powers (filters,
//                dichroic, dyes, imagers) and the lines' power ratios at the
//                sample. Every dSTORM and WideField rate is linear in the
//                excitation, so within a chapter a dye's rates are the
//                reference light's x mag x the local excitation factor, and its
//                dose in reference seconds carries its kinetics: tau = sum over
//                records of F x mag x footprint x depth x dt.
//                Grids: a chapter folds its old records into sparse
//                world-anchored grids of kLightTileUm, halved for a narrow
//                beam (LightLatticeUm: bilinear within kLightLatticeErr of the
//                peak), its lattice (G = sum of mag x
//                footprint x dt; G2 = sum of mag^2 x footprint^2 x dt, the primed
//                PALM conversion's), in its own sheared coordinates u = x - z
//                tan theta_s (HILO: the footprint at height z is the pivot
//                plane's moved by (z - z_p) tan theta_s, Excitation.h; a Flat
//                beam is not sheared: uniform but at its field stop). The
//                recent records (kLightRecent, kLightRecentSec) stay apart with
//                their footprints (PALM's activation steps); the open one is the
//                map's rate. A footprint is the beam profile at the nodes
//                (tile centres) down to kLightReach of its peak, a Flat beam's
//                1 over its field stop and one tile more; bilinear between
//                nodes in lerp form, so equal nodes read their value exactly.
//                Map (LightHistory::BuildMap): per structure GLOBAL (DNA-PAINT
//                and the off targets: T, the chapters' rows), DOSE (dSTORM,
//                WideField: epochs of lit chapters with equal rows, each
//                chapter's dose in seconds of its epoch reference's light, the
//                epochs' bounds the doses before them) or ACTIVATION (PALM:
//                the hazard as steps -- the merged past, then each recent
//                record -- then the open record's rate; blinks in T with the
//                chapters' rows). A fresh session's map is the uniform one
//                bit for bit (insiliscope.h): tau = T at a Flat beam in epi.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "DyeLibrary.h"
#include "Excitation.h"
#include "LightPath.h"
#include "ScopeMovie.h"

#include "insiliscope/insiliscope.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim {

constexpr double kLightTileUm = 0.25;        // node (i, j) at ((i + 1/2) h, (j + 1/2) h) of a chapter's coordinates
constexpr double kLightLatticeErr = 1e-3;    // the most a footprint's bilinear reading is off between nodes (x peak)
constexpr double kLightPoseTolUm = 0.025;    // a record keeps its first frame's pose while a frame stays this close
constexpr double kLightReach = 1e-4;         // a footprint reaches out to this fraction of its peak (0 beyond)
constexpr double kLightDepthStepUm = 0.005;  // TIRF's depth tables
constexpr int kLightRecent = 16;             // recent records kept apart; past it the oldest kLightMergeBatch merge
constexpr int kLightMergeBatch = 8;
constexpr double kLightRecentSec = 30.0;     // ... and those that ended longer ago, once the oldest is twice as old
constexpr double kLightLitMarginUm = 2.5;    // a Flat beam's field stop: the FOV and this margin

// A chapter's node spacing h under a beam: kLightTileUm, halved (to kLightTileUm / 16) while h^2 / 8 x the profile's
// largest curvature exceeds kLightLatticeErr (a 128 px FOV's FlatTop: 0.25 um; a 64 px one's: 0.125 um).
double LightLatticeUm(const Excitation& ex);

// Where a frame's light sits: the optical axis and a Flat beam's field stop (world um), the HILO pivot (the focal
// plane's height, um above the coverslip), and its share of a record (a drifting movie's frames).
struct LightPose
{
   double ax = 0, ay = 0, pivot = 0;
   double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
   double w = 1;
};

// A frame's light (MakeLightState), or a drifting movie's (MergeLightStates).
struct LightState
{
   bool lit = false;               // light reaches the sample (a line at it, not clipped, a source open)
   KineticEnv env;                 // ScopeKineticEnv of its spec
   std::string envKey, lightKey;   // a chapter's: the env but the laser keys; the geometry and the lines' ratios
   ExcitationSpec geo;
   std::shared_ptr<const Excitation> ex;
   std::vector<LaserLine> lines;   // at the sample (ScopeSampleLines)
   double total = 0;               // their sum, kW/cm^2
   std::vector<LightPose> poses;   // one, or a movie's frames' (weights summing to 1)
};
// The light of a frame of spec (its pose: the FOV centre pixel's centre, the focal plane): lightOn false (no source
// open): dark, with its env.
bool MakeLightState(const ScopeSpec& spec, bool lightOn, LightState& out, std::string& err);
// The pose of a frame of spec (MakeLightState's), g its excitation (ScopeExcitationSpec(spec)): a host moves a light
// state with it when only the stage moved (the excitation depends on the focus, not on the pose).
LightPose LightPoseOf(const ScopeSpec& spec, const ExcitationSpec& g);
// A drifting movie's light: its frames' states (one light, poses moving) as one state, the poses grouped as records
// group them, each weighted by its frames' share.
bool MergeLightStates(const std::vector<LightState>& frames, LightState& out, std::string& err);
// The same from the frames' poses alone (in order), base the first frame's light (a long stack: no state per frame).
bool MergeLightPoses(const LightState& base, const std::vector<LightPose>& poses, LightState& out, std::string& err);

// The host's copy of a clock map (core clockmap.h, the same arithmetic in its operand order): its buffer for
// isc_world_set_clock_map and what the engine reads from it.
struct MapGrid
{
   double x0 = 0, y0 = 0, h = 1;
   int32_t nx = 0, ny = 0;
   std::vector<double> v;
   double At(double u, double w) const;
};
struct MapDepth
{
   double z0 = 0, dz = 1;
   std::vector<double> v;
   double At(double z) const;
};
struct MapTerm
{
   int32_t grid = -1, depth = -1;
   double ox = 0, oy = 0, zp = 0, sx = 0, sy = 0, scale = 1;
};
using MapField = std::vector<MapTerm>;
struct MapStep
{
   double t0 = 0, t1 = 0, c = 0;
   MapField field;
};
// An epoch of a structure's clock: from tStart (T) on, these kinetics (ISC_KIN_*); xRef: DOSE's unit, the excitation
// per second of the epoch's reference light (its chapter's first lit record), 0 otherwise.
struct MapEpoch
{
   double tStart = 0;
   double kin[ISC_KIN_COUNT] = {};
   double xRef = 0;
   int chapter = -1;
   // That light's label physics under the current spec (the host's, not encoded).
   std::shared_ptr<const std::vector<LabelPhysics>> labels;
};
struct MapClock
{
   int mode = ISC_CLOCK_GLOBAL;
   std::vector<MapEpoch> epochs;
   std::vector<MapField> bound;   // DOSE: epochs.size() - 1
   MapField total, rate;
   double totalC = 0, rateC = 0, tOpen = 0;
   std::vector<MapStep> steps;    // ACTIVATION
   // The photon-budget bleaching of the structure's continuous state that has a budget (a WideField dye, a PALM pre
   // state) as a hazard -- each lit record's emission of that state at the dye over the budget, x its seconds -- by
   // tOpen (bleachTotal) and per second after (bleachRate). A dye bleaches when it reaches the dye's unit-exponential
   // draw (its window's aux): the core ends its windows there (clock map format 2). Empty: no budget.
   MapField bleachTotal, bleachRate;
};
class ClockMapView
{
public:
   std::vector<MapGrid> grids;
   std::vector<MapDepth> depths;
   std::vector<MapClock> s;       // ISC_STRUCT_COUNT
   double T = 0;                  // the sample's time it was made at (a movie under it starts there or later)
   uint64_t serial = 0;           // unique per map made (the engine's memos)
   std::vector<double> buffer;    // isc_world_set_clock_map's (Encode)
   void Encode();
   double Term(const MapTerm& t, double x, double y, double z) const;
   double Eval(const MapField& f, double x, double y, double z, double c = 0.0) const;
   // A dye's clock at tOpen and its rate (= the core's, isc_continuous_in_map), and its clock at T = t.
   double Clock(int st, double x, double y, double z) const;
   double Rate(int st, double x, double y, double z) const;
   double ClockAt(int st, double x, double y, double z, double t) const;
   // ACTIVATION: the hazard a dye at (x, y, z) has had by T = t (the steps, then the rate).
   double Hazard(int st, double x, double y, double z, double t) const;
};

class LightHistory
{
public:
   // dtSec of the sample's time under light (light.lit false: dark; its env still counts: DNA-PAINT's imagers).
   void Advance(double dtSec, const LightState& light);
   // The clock map of a movie that starts now (T) under light (lit false: dark), its dyes in [x0, x1] x [y0, y1]
   // (world um) at heights [z0, z1], with the labels of the spec cur. The light need not be the open record's: the map
   // then closes that at T and opens the light's, as Advance will once the movie's frames are counted (the same
   // numbers). While the open record extends under the same light, the map stays (its clocks run on from tOpen: a
   // movie after T reads it as well). Memoized (the last two: a live frame's and the prefetch's). nullptr (err) on a
   // bad spec.
   std::shared_ptr<const ClockMapView> BuildMap(const ScopeSpec& cur, const LightState& light, double x0, double y0,
                                                double x1, double y1, double z0, double z1, std::string& err);
   double T() const;
   // Whether light has reached the sample since the last Reset (the adapter starts the sample's time then: before it
   // every dye is fresh and DNA-PAINT's binding stationary, so a fresh device's stack starts at 0 however long it
   // ran idle).
   bool Lit() const;
   // The dose at (x, y) on the coverslip, seconds of the first lit chapter's reference light (F x mag x footprint, to
   // now): the background's fade; perSec (optional): the open record's per second (0 without light now).
   double DoseAt(double xUm, double yUm, double* perSec = nullptr) const;
   // The dose per second light gives at (x, y) on the coverslip, in DoseAt's units, were it on from now (a movie's
   // background fade): 0 when it is dark.
   double DoseRate(const LightState& light, double xUm, double yUm) const;
   // DoseAt at each (xy[2 i], xy[2 i + 1]) split into the dose of the light now's chapter (doseNow: its geometry, so its
   // heights scale as the light now's) and of the other chapters (doseOther), DoseRate there, one footprint for them
   // all, and the dose per second the light gives at its beam's peak (footprint 1; 0 when it is dark): a dose /
   // peakRate x the light's F is that past light as seconds at excitation 1 of the light now (Slide.h LightPastAt).
   void DosesAt(const LightState& light, const std::vector<double>& xy, std::vector<double>& doseNow,
                std::vector<double>& doseOther, std::vector<double>& rate, double& peakRate) const;
   void Reset();
   // For the checks and the logs: memory held (bytes), chapters, recent records (the open one too), chunks.
   size_t Bytes() const;
   size_t Chapters() const;
   size_t Records() const;

   // A footprint on a chapter's lattice: nodes [i0, i0 + nx) x [j0, j0 + ny).
   struct Footprint
   {
      long i0 = 0, j0 = 0;
      int nx = 0, ny = 0;
      std::vector<double> v;
      size_t Bytes() const { return v.capacity() * sizeof(double); }
   };

private:
   static constexpr int kChunk = 32;   // nodes per chunk side (8 um)
   static constexpr size_t kChunkNodes = static_cast<size_t>(kChunk) * kChunk;
   struct SparseGrid
   {
      struct Chunk
      {
         std::vector<double> g;   // G
         std::vector<float> g2;   // G2
      };
      std::unordered_map<uint64_t, Chunk> chunks;
      long i0 = 0, j0 = 0, i1 = -1, j1 = -1;   // the nodes held (i1 < i0: none)
      void Add(const Footprint& f, double a, double a2);
      double G(long i, long j) const;
      double G2(long i, long j) const;
      bool Empty() const { return i1 < i0; }
   };
   struct Chapter
   {
      double tStart = 0;
      std::string envKey, lightKey;
      KineticEnv env, refEnv;   // its first record's; its first lit record's
      bool lit = false;
      double refTotal = 0;
      std::shared_ptr<const Excitation> ex;
      bool flat = true, sheared = false;
      double tanS = 0, cosI = 1, F = 1, h = kLightTileUm;
      SparseGrid grid;          // the merged records
      double mergedSec = 0;     // their duration (lit or dark)
      // The labels of its light under the current spec (memo: the current spec's key).
      std::string labelsKey;
      std::shared_ptr<const std::vector<LabelPhysics>> labels;
   };
   struct Record
   {
      double t0 = 0, t1 = 0;
      size_t chapter = 0;
      bool lit = false;
      double mag = 0;
      std::vector<LightPose> poses;
      std::shared_ptr<const Footprint> fp;   // lit: on the chapter's lattice
   };
   static void SetReference(Chapter& c, const LightState& light);
   static bool SameChapter(const Chapter& c, const LightState& light);
   static bool SamePoses(const Chapter& c, const std::vector<LightPose>& a, const std::vector<LightPose>& b);
   static std::shared_ptr<const Footprint> MakeFootprint(const Chapter& c, const std::vector<LightPose>& poses);
   void CloseOpen();
   void Merge(const Record& r);
   static bool LabelsOf(Chapter& c, const ScopeSpec& cur, const std::string& curKey, std::string& err);
   std::shared_ptr<const ClockMapView> Build(const ScopeSpec& cur, const LightState& light, double x0, double y0,
                                             double x1, double y1, double z0, double z1, std::string& err);

   mutable std::mutex mutex_;
   double T_ = 0;
   uint64_t version_ = 0;   // + 1 at every record opened or closed and chapter started
   std::vector<Chapter> chapters_;
   std::vector<Record> recent_;   // closed, not merged, in time order
   bool haveOpen_ = false;
   Record open_;
   std::pair<std::string, std::shared_ptr<const ClockMapView>> memo_[2];   // most recent first
};

} // namespace sim
