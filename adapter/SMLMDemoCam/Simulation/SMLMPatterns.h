///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMPatterns.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Pattern -> continuous candidate-emitter-site sampler
//                abstraction. Each of the 9 patterns below samples ONE site
//                per call, at a continuous (real-valued) position -- no
//                discretized site list is ever built or cached, so there is
//                no precision/density-vs-memory-and-generation-time
//                tradeoff for THEM: positions are exact to floating-point
//                precision for free. Adding one of these means adding one
//                IPatternGenerator subclass and one CreatePattern() switch
//                arm -- nothing else changes.
//
//                Two deliberate exceptions to "no site list" exist:
//                CustomPointsPattern (external data, not re-derivable from a
//                formula) and, per SMLMStructures.h, SiteListPattern (used
//                for TiltedPlane/Uniform3D/Shell/NUP): a site there needs a
//                persistent IDENTITY across the whole movie -- labeling
//                efficiency ("this site is never labeled, ever") and NUP's
//                per-site linker displacement both require that, which a
//                fresh continuous draw per blink cannot express. That is a
//                genuine physical requirement, not a performance shortcut.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <cstdint>
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
   // Site-list 3D/NPC structures -- see SMLMStructures.h/.cpp.
   PATTERN_TILTED_PLANE = 9,
   PATTERN_UNIFORM_3D = 10,
   PATTERN_SHELL = 11,
   PATTERN_NUP = 12,
   // Always-on calibration bead grid -- see Calibration9SpotsPattern below.
   PATTERN_CALIBRATION_9_SPOTS = 13,
   // Site-list structure (SMLMStructures.cpp): webSMLM's default
   // filaments_ring -- three sinusoidal filaments whose z follows the same
   // sine as their y, plus an in-focus ring.
   PATTERN_FILAMENTS_RING = 14,
};

// A single candidate binding/emitter site, in micrometers (x,y), relative to
// the top-left corner of the simulated field of view.
struct EmitterSite
{
   double xUm = 0.0;
   double yUm = 0.0;
   // Depth relative to the focal plane, NANOMETRES (not um like x/y -- matches
   // every other z-scaled control in this codebase: StructureZRangeNm,
   // PsfZStepUm*1000, etc.). Defaults to 0 so every one of the 9 patterns
   // below that never sets it produces a purely 2D site, unchanged from
   // before this field existed. Added to the Z-stage's global focus offset
   // at render time -- see RenderPhotonImage in SMLMSimulation.h/.cpp.
   double zNm = 0.0;
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

   // Opt-in escape hatch from the blinking model entirely: a pattern that
   // returns true here describes a FIXED set of emitters that are ON in
   // every frame of the movie (a calibration bead field), and EmitterModel
   // (SMLMSimulation.h) then bypasses its Poisson-arrival/exponential-
   // ON-lifetime process for it completely -- EmitterDensityPerSec and
   // OnLifetimeSec stop having any effect, and SampleSite() is never
   // called. Every other pattern leaves this at the default false and is
   // driven by the blinking process exactly as before.
   //
   // Deliberately a property of the PATTERN rather than a separate
   // "AlwaysOn" MM property: whether the specimen blinks is intrinsic to
   // what the specimen IS here (beads vs. dye), and an "always on" toggle
   // applied to, say, the NUP structure would just be a very bright blob,
   // not a meaningful configuration.
   virtual bool AlwaysOnSites(double /*widthUm*/, double /*heightUm*/,
                               std::vector<EmitterSite>& /*out*/) const { return false; }
};

// The classic resolution-test progression (easiest to hardest gap/spacing,
// in nanometers) used as the default for CirclePattern/SpiralPattern/
// StarPattern/HeartPattern/ResolutionTargetPattern below when no explicit
// list is supplied. Exposed to the user as the ResolutionSpacingsNm
// property (a comma-separated list) so it's changeable without a rebuild.
std::vector<double> DefaultResolutionSpacingsNm();

// Round-trip helpers for the ResolutionSpacingsNm property: Format writes
// space-separated values; Parse accepts spaces, commas or semicolons
// (MMCore rejects a comma in any property value SET through it, so the
// property itself is space-separated).
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

// A 3x3 grid of ALWAYS-ON emitters ("beads") centered on the FOV, ported
// from webSMLM's generateCalibrationStack() bead field -- the specimen a
// real astigmatic-PSF z-calibration acquisition uses (a coverslip of
// fiducial beads, stage-scanned through focus), as opposed to a blinking
// dye. Being always-on is what makes it useful: drive SMLMDemoZStage
// through focus and every frame shows the same nine spots at the same x,y,
// with only the PSF shape changing.
//
// Unlike webSMLM's version, the spacing here is a plain fraction of the FOV
// (quarter of the smaller side, so the grid spans half the frame) rather
// than being derived from the PSF kernel's own pixel half-width: this
// pattern is constructed by CreatePattern(), which has no visibility into
// the PsfGeneratorRequest, and a quarter-FOV spacing already clears any
// kernel this device will realistically be configured with.
//
// SampleSite() is still implemented (uniform pick among the nine) so this
// class remains a well-behaved IPatternGenerator, but EmitterModel never
// calls it -- see AlwaysOnSites() above.
class Calibration9SpotsPattern : public IPatternGenerator
{
public:
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return "Calibration9Spots"; }
   bool AlwaysOnSites(double widthUm, double heightUm, std::vector<EmitterSite>& out) const override;
};

// Wraps any continuous 2D pattern above and gives its sites a uniform z
// spread in [-zRangeNm, +zRangeNm] -- a decorator rather than a zRange
// member on all 9 patterns, since the 2D geometry is what each pattern is
// ABOUT and z is orthogonal to it: imposing one z formula on all 9 would be
// 9 arbitrary choices rather than one honest "the object also has
// thickness". Consumes ZERO extra rng draws when zRangeNm <= 0 (early
// return in SampleSite), so StructureZRangeNm=0 leaves every wrapped
// pattern's random stream bit-for-bit unchanged from before this class
// existed -- CreatePattern below only wraps when zRangeNm > 0.
class ZSpreadPattern : public IPatternGenerator
{
public:
   ZSpreadPattern(std::unique_ptr<IPatternGenerator> inner, double zRangeNm)
      : inner_(std::move(inner)), zRangeNm_(zRangeNm) {}
   EmitterSite SampleSite(double widthUm, double heightUm, std::mt19937_64& rng) const override;
   const char* Name() const override { return inner_->Name(); }

private:
   std::unique_ptr<IPatternGenerator> inner_;
   double zRangeNm_;
};

// Membrane/pore-axis orientation for PATTERN_NUP -- see SMLMStructures.h.
enum class MembraneOrientation
{
   TopDown = 0,
   Sideways = 1,
};

// Everything a 3D/NPC structure builder (SMLMStructures.h) needs, snapshotted
// by value once per config change (never read per frame). zRangeNm=0 is the
// "flat/2D" default for the continuous patterns above (via ZSpreadPattern);
// the site-list structures (SMLMStructures.h) interpret it/structureSizeNm
// per their own geometry -- see each builder's own doc comment.
//
// NOTE: this struct's own default (0 = flat/2D, matching every existing
// pre-3D seed's output exactly) is deliberately NOT the same as the
// StructureZRangeNm MM property's user-facing default (500nm, so 3D is
// visible out of the box once that property exists) -- the two defaults
// are decoupled on purpose so a default-constructed StructureParams (used
// at every CreatePattern call site until the MM property wiring lands)
// can never silently change existing seeds' output.
struct StructureParams
{
   double zRangeNm = 0.0;               // +/- half-extent for TiltedPlane/Uniform3D and the ZSpreadPattern decorator
   double structureSizeNm = 500.0;      // Shell radius only
   double labelingEfficiencyPct = 100.0; // Site-list structures only -- see SMLMStructures.h

   // NUP (Thevathasan et al. 2019 Nup96 NPC; geometry parametrized per
   // Wanninger et al. 2023 "CIR4MICS") -- see SMLMStructures.cpp for the
   // full geometry this drives.
   double nupRadiusNm = 53.5;
   double nupCornerSpreadNm = 12.0;
   double nupRingSeparationNm = 50.0;
   double nupLinkerMinNm = 2.0;
   double nupLinkerMaxNm = 5.0;
   MembraneOrientation nupMembrane = MembraneOrientation::TopDown;
   int nupCount = 20;
   double nupMinSpacingNm = 200.0;
   double nupCurvatureNm = 150.0;
};

// structure/widthUm/heightUm/structureSeed are only consumed by the site-
// list structure types added in SMLMStructures.h (TiltedPlane/Uniform3D/
// Shell/NUP) -- for the 9 patterns above, only structure.zRangeNm matters
// (via the ZSpreadPattern wrap below), and widthUm/heightUm/structureSeed
// are unused. Defaulted so every pre-existing call site keeps compiling
// unchanged.
std::unique_ptr<IPatternGenerator> CreatePattern(SMLMPatternType type, const std::string& customPointsFile,
                                                  const std::vector<double>& spacingsNm = DefaultResolutionSpacingsNm(),
                                                  const StructureParams& structure = StructureParams(),
                                                  double widthUm = 0.0, double heightUm = 0.0,
                                                  uint64_t structureSeed = 0);

} // namespace sim
