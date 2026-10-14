///////////////////////////////////////////////////////////////////////////////
// FILE:          LiveScope.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Live imaging of one sample, frame by frame, from ScopeMovie
//                specs: the Micro-Manager camera's live mode
//                (SMLMImageGeneration.cpp LiveProducerLoop) without the
//                device. The sample keeps its history: the illumination
//                history (IlluminationHistory: seconds of light per 0.25 um
//                tile, per epoch of rates), so dyes bleach, convert and shelve
//                where the light has been and stay fresh elsewhere; the
//                drift walker (Drift.h), from 0 on a new world; the camera's
//                live noise (its own seed, a frame counter). Each frame is
//                RenderScopePhotons at the drifted pose with the history's
//                clocks (running images carried from frame to frame), then
//                ApplyNoiseChain. Used by the viewer (cli/scope_wasm.cpp
//                isc_live_*). Not here: the adapter's GPU splat, render-ahead,
//                BrightField focus prefetch, illumination profile and the
//                background fade (MM properties with no ScopeMovie option).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Drift.h"
#include "IlluminationHistory.h"
#include "SMLMNoise.h"
#include "SampleSource.h"
#include "ScopeMovie.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

struct LiveFrameInfo
{
   unsigned width = 0, height = 0;
   double tSec = 0;          // the sample's time at the start of the frame (s since the world was made)
   double litSec = 0;        // illumination the FOV centre had before the frame (the history's clock)
   bool lit = false;         // the frame lit the sample (added to the history)
   bool epi = false, trans = false;
   size_t blinks = 0;
   long dyes = 0;
   DriftNm drift;            // the sample's drift at the frame (nm)
   double renderMs = 0;
   // How the frame's fluorescence was drawn (FluorescenceMovie::Render's "frames" progress): a JSON array of
   // strings, e.g. ["SMLM: 120 blinks (splat)", "initial ON: mean-field (FFT)"]; [] for a lamp-only or dark frame.
   std::string paths = "[]";
};

class LiveScope
{
public:
   // One frame of the spec (frames is ignored: one) at its pose (x, y, z) and exposure. countLight: the frame's
   // light goes into the history (the adapter counts a frame that is taken with the lasers on; a host that shows
   // every frame passes true). A spec of another world (seed, cell or specimen parameters) starts a fresh sample.
   bool Frame(const ScopeSpec& spec, bool countLight, std::vector<uint16_t>& adu, LiveFrameInfo& info,
              std::string& err);
   // A fresh sample: no light so far, no drift, time 0.
   void Reset();
   double TimeSec() const { return tSec_; }
   const IlluminationHistory& History() const { return history_; }
   // The lit rect of a frame (the adapter's LitRect): the FOV plus the query margin (2 um) and 0.5 um, so the
   // mean-field grid and the 0.25 um tiles lie wholly inside it.
   static void LitRect(double xUm, double yUm, double wUm, double hUm, double& x0, double& y0, double& x1, double& y1);

private:
   IlluminationHistory history_;
   DriftWalker drift_;
   SampleSettings world_;
   bool haveWorld_ = false;
   double tSec_ = 0;
   uint32_t noiseCounter_ = 0;
   // the camera's fixed-pattern maps (a function of the seed and the camera settings) and their key
   PixelOffsetMap offsetMap_;
   PixelGainMap gainMap_;
   PixelReadNoiseMap readNoiseMap_;
   std::vector<double> noiseKey_;
};

} // namespace sim
