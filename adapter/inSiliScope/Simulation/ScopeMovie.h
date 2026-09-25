///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeMovie.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   A short cell-field movie without Micro-Manager: the camera's
//                precomputed CellField stack pipeline (CellFieldSource ->
//                BucketEventsByFrame -> RenderPhotonImage -> ApplyNoiseChain,
//                same seed streams) for one FOV. Used by cli/ (TIFF files)
//                and the viewer (web/, via the WASM export isc_scope_movie).
//                Gaussian PSF only (the vectorial models need the adapter's
//                embedded JVM bridge).
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sim {

struct ScopeOption
{
   const char* name;
   double value;       // default
   const char* help;
};

// Every named option with its default (the adapter's property defaults).
const std::vector<ScopeOption>& ScopeMovieOptions();

// Option values by name; missing ones take their default. Besides the named
// options, "p.<name>" passes a core world parameter through (the prototype's
// names, e.g. p.mtWobbleTurn); unknown p.* names are ignored.
using ScopeSpec = std::map<std::string, double>;

// False for a name that is neither a named option nor p.*.
bool ScopeSpecSet(ScopeSpec& spec, const std::string& name, double value);
double ScopeSpecGet(const ScopeSpec& spec, const char* name);
// "k=v k=v ..." (spaces, commas or semicolons); false (with err) on a bad token.
bool ParseScopeSpec(const std::string& text, ScopeSpec& spec, std::string& err);

struct ScopeMovieInfo
{
   unsigned width = 0, height = 0;
   long frames = 0;
   size_t blinks = 0;
   double querySec = 0, totalSec = 0;
   std::string description;   // one line of the settings, for file metadata
};

// Frame size and count of a spec (no rendering).
void ScopeMovieDims(const ScopeSpec& spec, unsigned& w, unsigned& h, long& frames);

// Renders the movie, calling onFrame(f, adu) for f = 0..frames-1 (return
// false to stop). False (with err) on a failure.
bool RenderScopeMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                      ScopeMovieInfo& info, std::string& err);

} // namespace sim
