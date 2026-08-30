///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMPatterns.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Pattern -> continuous candidate-emitter-site sampler
//                abstraction. Each pattern samples ONE site per call, at a
//                continuous (real-valued) position -- no discretized site
//                list is ever built or cached, so there is no
//                precision/density-vs-memory-and-generation-time tradeoff:
//                positions are exact to floating-point precision for free.
//                Adding a new pattern means adding one IPatternGenerator
//                subclass and one CreatePattern() switch arm -- nothing else
//                changes.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <memory>
#include <random>
#include <string>
#include <vector>

namespace sim {

enum SMLMPatternType
{
   PATTERN_CIRCLE = 0,
   PATTERN_LINES = 1,
   PATTERN_GRID = 2,
   PATTERN_RANDOM = 3,
   PATTERN_CUSTOM_POINTS = 4,
   PATTERN_SPIRAL = 5,
   PATTERN_STAR = 6,
   PATTERN_HEART = 7,
   PATTERN_RESOLUTION_TARGET = 8,
};

// A single candidate binding/emitter site, in micrometers, relative to the
// top-left corner of the simulated field of view.
struct EmitterSite
{
   double xUm = 0.0;
   double yUm = 0.0;
};

class IPatternGenerator
{
public:
   virtual ~IPatternGenerator() = default;

   // Samples ONE candidate emitter site, continuously, for the field of
   // view (widthUm x heightUm). Called once per spawned blink event (a few
   // hundred to a few thousand times per movie/session, not per frame), so
   // doing real work here (weighted picks, trig, an occasional file lookup)
   // is cheap in aggregate -- there is no per-call performance pressure
   // that would justify precomputing and caching a discretized list.
   virtual EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const = 0;

   virtual const char* Name() const = 0;
};

// The classic resolution-test progression (easiest to hardest gap/spacing,
// in nanometers) used as the default for CirclePattern/SpiralPattern/
// StarPattern/HeartPattern/ResolutionTargetPattern below when no explicit
// list is supplied. Exposed to the user as the ResolutionSpacingsNm
// property (a comma-separated list) so it's changeable without a rebuild.
std::vector<double> DefaultResolutionSpacingsNm();

// Comma-separated round-trip helpers for the ResolutionSpacingsNm property.
// ParseResolutionSpacingsNm skips unparsable/non-positive tokens; returns an
// empty vector if nothing valid was found (caller should then leave the
// existing value in place rather than applying an empty spacing list).
std::string FormatResolutionSpacingsNm(const std::vector<double>& spacingsNm);
std::vector<double> ParseResolutionSpacingsNm(const std::string& text);

class CirclePattern : public IPatternGenerator
{
public:
   explicit CirclePattern(std::vector<double> spacingsNm = DefaultResolutionSpacingsNm())
      : spacingsNm_(spacingsNm.empty() ? DefaultResolutionSpacingsNm() : std::move(spacingsNm)) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Circle"; }

private:
   std::vector<double> spacingsNm_;
};

class LinesPattern : public IPatternGenerator
{
public:
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Lines"; }
};

class GridPattern : public IPatternGenerator
{
public:
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Grid"; }
};

class RandomPattern : public IPatternGenerator
{
public:
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Random"; }
};

// Circle/Star/Heart/Spiral all render as N concentric rings/scaled copies/
// spiral-arcs (N = spacingsNm.size(), default 9), evenly spaced radially/
// by-scale from just outside center to the FOV edge. Each is TWO continuous
// curves (not one), radially separated by one value from the
// (user-changeable) spacing list -- the SMALLEST ring/star/heart/innermost-
// arc gets the SMALLEST spacing, the LARGEST gets the LARGEST, in the order
// given: reverse the list to invert that. Ring/copy/arc selection is
// weighted by (exact or closely-approximate) arc length, so longer curves
// receive proportionally more emitters over time, same as uniform line
// density along a real two-line target. See DefaultResolutionSpacingsNm()
// above, shared with ResolutionTargetPattern below.

class SpiralPattern : public IPatternGenerator
{
public:
   explicit SpiralPattern(std::vector<double> spacingsNm = DefaultResolutionSpacingsNm())
      : spacingsNm_(spacingsNm.empty() ? DefaultResolutionSpacingsNm() : std::move(spacingsNm)) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Spiral"; }

private:
   std::vector<double> spacingsNm_;
};

class StarPattern : public IPatternGenerator
{
public:
   explicit StarPattern(std::vector<double> spacingsNm = DefaultResolutionSpacingsNm())
      : spacingsNm_(spacingsNm.empty() ? DefaultResolutionSpacingsNm() : std::move(spacingsNm)) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Star"; }

private:
   std::vector<double> spacingsNm_;
};

class HeartPattern : public IPatternGenerator
{
public:
   explicit HeartPattern(std::vector<double> spacingsNm = DefaultResolutionSpacingsNm())
      : spacingsNm_(spacingsNm.empty() ? DefaultResolutionSpacingsNm() : std::move(spacingsNm)) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Heart"; }

private:
   std::vector<double> spacingsNm_;
};

// N-cell grid of sub-FOVs (N = spacingsNm.size(), default 9, arranged as
// close to square as possible), each drawing a row of continuous vertical
// lines spaced by one value from the (user-changeable) spacing list -- a
// classic 1D line-spacing resolution test: scan cell to cell to see the
// density/PSF/pixel-size combination's practical resolving power.
class ResolutionTargetPattern : public IPatternGenerator
{
public:
   explicit ResolutionTargetPattern(std::vector<double> spacingsNm = DefaultResolutionSpacingsNm())
      : spacingsNm_(spacingsNm.empty() ? DefaultResolutionSpacingsNm() : std::move(spacingsNm)) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "ResolutionTarget"; }

private:
   std::vector<double> spacingsNm_;
};

// Loads a point list from a CSV file with one "x,y" pair per line, values in
// the range [0, 1] normalized to the FOV (x=0..1 left..right, y=0..1
// top..bottom), lazily on first use and cached (this is the one pattern
// where caching a list is actually necessary: the points are external data,
// not something re-derivable from a formula). Lines that fail to parse as
// two comma-separated numbers are skipped. If the file cannot be read or
// contains no valid points, falls back to CirclePattern's output so the
// camera still produces something.
class CustomPointsPattern : public IPatternGenerator
{
public:
   explicit CustomPointsPattern(std::string filePath) : filePath_(std::move(filePath)) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "CustomPoints"; }

private:
   std::string filePath_;
   mutable std::vector<EmitterSite> cachedNormalizedPoints_; // x,y both in [0,1]
   mutable bool loaded_ = false;
};

std::unique_ptr<IPatternGenerator> CreatePattern(SMLMPatternType type, const std::string& customPointsFile,
                                                  const std::vector<double>& spacingsNm = DefaultResolutionSpacingsNm());

} // namespace sim
