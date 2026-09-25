///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMStructures.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Site-list 3D/NPC structure builders (TiltedPlane/Uniform3D/
//                Shell/NUP/FilamentsRing) plus the SiteListPattern that turns a
//                pre-generated, finite site list into an IPatternGenerator
//                -- see SMLMPatterns.h's own header comment for why a
//                persistent site list is needed here, unlike the 9
//                continuous patterns there.
//
//                Ported from the webSMLM reference simulator's
//                buildStructure()/buildNupStructure() (webSMLM/index.html)
//                -- see PARITY.md in that project for the parameter/feature
//                correspondence this project tracks against it.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "SMLMPatterns.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace sim {

// A pattern that owns a FINITE, PRE-GENERATED site list and implements
// SampleSite() by picking a uniform random index into it -- one rng draw
// (uniform_int_distribution) plus a vector index, cheaper per blink than
// any of the 9 continuous patterns in SMLMPatterns.h/.cpp, all of which do
// at least one weighted pick and some trig. widthUm/heightUm are ignored
// (sites are already absolute um/nm, fixed at build time) -- a FOV change
// therefore requires a rebuild, which already happens automatically: see
// BuildStructurePattern's caller in SMLMImageGeneration.cpp, invoked from
// the same InvalidateStack()-gated path that rebuilds the PSF kernel cache.
class SiteListPattern : public IPatternGenerator
{
public:
   SiteListPattern(std::vector<EmitterSite> sites, const char* name);
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return name_; }
   size_t SiteCount() const { return sites_.size(); }

private:
   std::vector<EmitterSite> sites_;
   const char* name_;
};

// Builds the site list for one of the 3D structure types (PATTERN_TILTED_
// PLANE/PATTERN_UNIFORM_3D/PATTERN_SHELL/PATTERN_NUP/PATTERN_FILAMENTS_RING),
// applies the
// labeling-efficiency keep/drop filter, and returns the finished pattern.
// type must be one of the four site-list types above; any other value
// returns nullptr (callers should not reach this function otherwise --
// see CreatePattern in SMLMPatterns.cpp, which dispatches here only for
// those four).
//
// structureSeed seeds this function's OWN, independent
// std::mt19937_64 -- deliberately NOT the caller's arrival/noise rng, so
// building a site list (however many draws that takes) can never shift
// the blink-arrival/noise-map random stream for any configuration. Pass
// e.g. (uint64_t)RandomSeed XOR'd with a fixed constant distinct from the
// live-mode seed's own XOR constant -- see SMLMImageGeneration.cpp's call
// sites.
//
// outKeptSites/outTotalSites (optional): the labeling filter's effect, for
// corelog reporting.
std::unique_ptr<IPatternGenerator> BuildStructurePattern(
      SMLMPatternType type, double widthUm, double heightUm,
      const StructureParams& sp, uint64_t structureSeed,
      size_t* outKeptSites = nullptr, size_t* outTotalSites = nullptr);

// Half-extent in Z (nm) the built structure actually reaches -- NOT simply
// structure.zRangeNm (Shell is sized by its own radius; NUP adds ring
// separation and curvature on top of its own flat geometry). Used to warn
// when a structure's own z extent will outrun the PSF kernel's cached z
// range (see RenderPhotonImage's per-emitter clamp counters). type must be
// one of the four site-list types; any other value returns 0.
double StructureZExtentNm(SMLMPatternType type, const StructureParams& sp);

} // namespace sim
