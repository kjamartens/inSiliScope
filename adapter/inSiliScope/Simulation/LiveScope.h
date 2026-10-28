///////////////////////////////////////////////////////////////////////////////
// FILE:          LiveScope.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Live imaging of one sample, frame by frame, from ScopeMovie
//                specs: the Micro-Manager camera's live mode
//                (SMLMImageGeneration.cpp LiveProducerLoop) without the
//                device. The sample keeps its history: the light history
//                (LightHistory.h: what light every place has had, exactly;
//                each dye reads its clock at its own place from its map), so
//                dyes bleach, convert and shelve where the light has been, at
//                the beam's own shape, and stay fresh elsewhere; the drift
//                walker (Drift.h), from 0 on a new world; the camera's live
//                noise (its own seed, a frame counter). Each frame is
//                RenderScopePhotons at the drifted pose on the history
//                (running images carried from frame to frame), then
//                ApplyNoiseChain. Used by the viewer (cli/scope_wasm.cpp
//                isc_live_*). Not here: the adapter's GPU splat, render-ahead,
//                BrightField focus prefetch and the background fade (MM
//                properties with no ScopeMovie option).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Drift.h"
#include "LightHistory.h"
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
   double litSec = 0;        // the dose the FOV centre had before the frame (LightHistory::DoseAt: seconds of the
                             // first light at its peak)
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
   // The next n frames at once (a recording: no pacing), adu = n frames one after another. Without drift and with
   // frames that light the sample, one movie of n frames on the history's map (one setup and one events query, as
   // the adapter's render-ahead); else n Frame calls. info: the first frame's tSec and litSec, the last one's drift,
   // blinks and dyes summed, the whole call's renderMs.
   bool Frames(const ScopeSpec& spec, long n, bool countLight, std::vector<uint16_t>& adu, LiveFrameInfo& info,
               std::string& err);
   // What Frames(spec, n, countLight) does to the sample (its light history, drift, time and noise counter), without
   // rendering: a saved session's light replayed (docs/dev/SAVE_SETTINGS.md), so the next frame is the one the session
   // would have rendered, bit for bit.
   bool Advance(const ScopeSpec& spec, long n, bool countLight, std::string& err);
   // A fresh sample: no light so far, no drift, time 0.
   void Reset();
   double TimeSec() const { return tSec_; }
   const LightHistory& History() const { return history_; }

private:
   struct Step;
   // The frames' spec, pose and light (the world reset on a new world); 0 = take them one frame at a time (n > 1 with
   // drift or with frames that do not light the sample), -1 = an error.
   int Begin(const ScopeSpec& specIn, long n, bool countLight, Step& st, std::string& err);
   // Their light into the history, the time and the drift on, the noise counter on (after the frames are rendered).
   void Finish(const Step& st, long n);

   LightHistory history_;
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
