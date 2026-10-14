///////////////////////////////////////////////////////////////////////////////
// FILE:          IlluminationHistory.h
// PROJECT:       insiliscope
//-----------------------------------------------------------------------------
// DESCRIPTION:   The adapter's world-anchored illumination history: how many
//                seconds of illumination each place of the sample has had
//                (0.25 um tiles, weighted by the excitation there), so
//                imaging a region bleaches (and uses up) its dyes there and
//                nowhere else: bleach, move away and come back, and it is
//                still dim; a region never lit starts fresh at clock 0.
//
//                And under which rates (2026-10-08): each tile also keeps its
//                rate history, the epochs (KineticEnv: the light path's and
//                the labels' kinetic inputs) it was lit in and the clock at
//                which each began, as a node of a shared trie (tiles with the
//                same past share one node). A movie queries each dye with the
//                rates of its tile's past (DyeClock::Segments, core ABI 11),
//                so a change of laser power or dye acts from now on.
//
//                And in height (2026-10-13, the excitation geometry,
//                Excitation.h): each tile keeps a clock per z band (kBands
//                world bands above the coverslip), so TIRF or HILO light
//                bleaches the heights it reaches: TIRF, then epi, and the dyes
//                higher in the cell are fresh. Under a light that is the same
//                at every height (epi) the bands stay equal and a snapshot
//                keeps one (outputs as before).
//
//                FluorescenceMovie reads each dye at its tile's clock
//                (ScopeMovie.h DyeClock); the live loop and stacks advance the
//                lit rect (the FOV and its 2 um margin, what a movie renders)
//                as their frames are made. Not used by the cli or the viewer
//                (their movies start at start-sec, the kinetics at the local
//                light).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "ScopeMovie.h"
#include "Excitation.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sim {


// A frame's light per tile and band: w[b] (b < kHistoryBands) at the tile centre (world um), the excitation relative to
// the lasers' power at the sample, quantised by the caller. Empty: 1 everywhere.
using HistoryLight = std::function<void(double xUm, double yUm, float* w)>;
// HistoryLightField's (Excitation.h) light with the optical axis at world (axisXUm, axisYUm), snapped to the tile grid
// (HistoryAxisUm). Empty when the excitation is uniform.
HistoryLight HistoryLightFor(const Excitation& ex, const std::vector<ExcitationLine>& shares, double axisXUm,
                             double axisYUm);

// The clocks and rate histories of a rect, copied out of the history (no lock
// while a movie reads it). Positions outside the rect read clock 0 with the
// snapshot's epoch from 0 (a fresh place).
class ClockSnapshot : public DyeClock
{
public:
   double At(double xUm, double yUm) const override;
   void KeyAt(double xUm, double yUm, double& tSec, uint32_t& history) const override;
   void KeyAtZ(double xUm, double yUm, double zUm, double& tSec, uint32_t& history) const override;
   const std::vector<double>& BandEdges() const override { return edges; }
   double TileUm() const override { return tileUm; }
   void Segments(uint32_t history, std::vector<ClockSegment>& out) const override;
   void Regions(double x0Um, double y0Um, double x1Um, double y1Um, std::vector<ClockRegion>& out) const override;
   // Adds dtSec x light to the tiles of the snapshot whose centre lies in the
   // rect, with IlluminationHistory::Advance's arithmetic: the snapshot as the
   // history will be once a frame lit there is taken (render-ahead; the frames
   // are in the snapshot's epoch, so the histories stay).
   void Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec, const HistoryLight& light);
   long ix0 = 0, iy0 = 0;
   unsigned nx = 0, ny = 0, nb = 1;   // nb: 1 (every band alike) or kHistoryBands
   double tileUm = 0.25;
   std::vector<double> edges;   // the band edges when nb > 1, else empty
   std::vector<float> t;        // nx x ny x nb, x fastest, then y, then the band
   std::vector<uint32_t> node;  // the tiles' histories (trie nodes), with the snapshot's epoch last
   uint32_t freshNode = 0;      // outside the snapshot
   // The segments of each history in the snapshot (the envs kept alive here).
   std::unordered_map<uint32_t, std::vector<ClockSegment>> segments;
   std::vector<std::shared_ptr<const KineticEnv>> envs;

private:
   void Expand();   // nb 1 -> kHistoryBands (band 0 copied)
   size_t Index(long ix, long iy, unsigned b) const
   {
      return static_cast<size_t>(ix) + static_cast<size_t>(nx) * (static_cast<size_t>(iy) + static_cast<size_t>(ny) * b);
   }
};

class IlluminationHistory
{
public:
   static constexpr double kTileUm = kHistoryTileUm;
   // The id of an epoch equal to env (ScopeKineticEnv), registered if new.
   uint32_t RegisterEpoch(const KineticEnv& env);
   // Adds dtSec x light(tile centre) to every tile whose centre lies in the
   // rect (world um), per band, lit in epoch: a tile last lit in another epoch
   // starts a segment at its clock (or replaces its last one, if that is empty).
   void Advance(double x0Um, double y0Um, double x1Um, double y1Um, double dtSec, const HistoryLight& light,
                uint32_t epoch);
   // The rect's clocks, each tile's history continued in epoch (what a frame
   // lit in it sees).
   ClockSnapshot Snapshot(double x0Um, double y0Um, double x1Um, double y1Um, uint32_t epoch);
   // The clock at a point at the coverslip (band 0; the background fade).
   double ClockAt(double xUm, double yUm) const;
   void Reset();
   size_t Chunks() const;

private:
   static constexpr int kChunk = 32;   // tiles per chunk side (8 um)
   static constexpr size_t kChunkTiles = static_cast<size_t>(kChunk) * kChunk;
   static uint64_t Key(long cx, long cy)
   {
      return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
   }
   struct Chunk
   {
      std::vector<float> t;        // kChunk x kChunk x kHistoryBands seconds, x fastest, the band last
      std::vector<uint32_t> node;  // ... their histories (0: never lit)
      // False: every band equals band 0 (a light alike in height has lit it, or none), and only band 0 is kept: a
      // frame then advances one value per tile, not kHistoryBands (live frames: 1.5 -> 0.5 ms).
      bool banded = false;
      void Expand();
   };
   // A trie node: its parent's segments, then epoch from clock tStart on.
   // Node 0 is the root (no segments).
   struct Node
   {
      uint32_t parent = 0;
      float tStart = 0;
      uint32_t epoch = 0;
   };
   // The node continuing `node` (at clock t) in epoch (callers hold mutex_).
   uint32_t Continue(uint32_t node, float t, uint32_t epoch);
   uint32_t Intern(uint32_t parent, float tStart, uint32_t epoch);
   void SegmentsOf(uint32_t node, std::vector<ClockSegment>& out) const;
   mutable std::mutex mutex_;
   std::unordered_map<uint64_t, Chunk> chunks_;
   std::vector<Node> nodes_ = { Node() };
   std::unordered_multimap<uint64_t, uint32_t> nodeByKey_;   // hash of (parent, tStart, epoch) -> nodes
   std::vector<std::shared_ptr<const KineticEnv>> epochs_;
};

} // namespace sim
