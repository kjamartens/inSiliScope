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
//                PSF: GibsonLanniZernike (the adapter's default, C++
//                ZernikePsf.h; psf-* options = the PSFParam_ properties) or
//                Gaussian (psf-model 0); RichardsWolf/GibsonLanni need the
//                adapter's JVM. modality = 1 renders WideField
//                (WidefieldRender.h) instead of the blinks.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
// names, e.g. p.mtWobbleTurn); unknown p.* names are ignored. "zern.<j>"
// (j = 0..27) sets Zernike coefficient j in waves, replacing the preset's.
using ScopeSpec = std::map<std::string, double>;

// False for a name that is neither a named option nor p.* / zern.*.
bool ScopeSpecSet(ScopeSpec& spec, const std::string& name, double value);
double ScopeSpecGet(const ScopeSpec& spec, const char* name);
// An option's value from text: a number, or a name (modality: SuperRes,
// WideField; psf-model: Gaussian, GibsonLanniZernike, ...; psf-mask: None,
// DoubleHelix; psf-interp: Nearest, Linear, Cubic, Fft; psf-zernike-preset:
// the PSFParam_PsfZernikePreset names). False if neither.
bool ScopeOptionValue(const std::string& name, const char* text, double& value);
// "k=v k=v ..." (spaces, commas or semicolons); false (with err) on a bad token.
bool ParseScopeSpec(const std::string& text, ScopeSpec& spec, std::string& err);

struct ScopeMovieInfo
{
   unsigned width = 0, height = 0;
   long frames = 0;
   size_t blinks = 0;
   long dyes = 0;            // WideField: labelled dyes on the dye grid
   double halfTimeSec = 0;   // WideField: bleaching half time at pattern peak (inf = never)
   double querySec = 0, totalSec = 0;
   std::string description;   // one line of the settings, for file metadata
};

struct PsfGeneratorRequest;
struct PsfKernelCache;
// The spec's PSF as the camera's BuildPsfGeneratorRequest would build it from
// the same PSFParam_ values. False for psf-model 0 (Gaussian; err empty) or a
// bad value (err set).
bool ScopePsfRequest(const ScopeSpec& spec, PsfGeneratorRequest& req, std::string& err);
// Its kernel (memoized, ComputePsfKernelCache); cache.valid = false and true
// returned for the Gaussian.
bool ScopePsfKernel(const ScopeSpec& spec, PsfKernelCache& cache, std::string& err);

// Frame size and count of a spec (no rendering).
void ScopeMovieDims(const ScopeSpec& spec, unsigned& w, unsigned& h, long& frames);

// Renders the movie, calling onFrame(f, adu) for f = 0..frames-1 (return
// false to stop). False (with err) on a failure.
class WidefieldScene;

// A WideField movie in steps: Begin (the world, the scene at its focus, the
// bleach basis anchored at the first frame), then -- in GPU mode, images
// deferred -- the images from a GPU host (Scene().MakeGpuJob / SetImages, the
// viewer's WebGPU) or ComputeCpuImages(), then Render (background, dyes,
// noise per frame). RenderScopeMovie runs the steps on the CPU.
class WidefieldMovie
{
public:
   WidefieldMovie();
   ~WidefieldMovie();
   bool Begin(const ScopeSpec& spec, bool gpuMode, std::string& err);
   WidefieldScene& Scene();
   void ComputeCpuImages();
   bool Render(const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame, ScopeMovieInfo& info,
               std::string& err);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

bool RenderScopeMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                      ScopeMovieInfo& info, std::string& err);

} // namespace sim
