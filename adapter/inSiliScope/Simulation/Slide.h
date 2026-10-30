///////////////////////////////////////////////////////////////////////////////
// FILE:          Slide.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The fluorescent slide (specimen 3, data/specimens.json Slide;
//                spec/ALGORITHM.md "Fluorescent slide"): a uniform layer of
//                dye, slide-thickness-um thick on the coverslip, at
//                slide-conc-micromolar of each of its dyes (Mix: TetraSpeck's
//                four components as equivalent dyes, data/fiducials; or one
//                library dye's main state). The core has no geometry for it.
//
//                Its light per pixel is the free imager's model
//                (Excitation::ImagerMap): every height of the layer, lit by the
//                beam, collected defocused onto the pixel -- exact for an
//                unbleached layer. Bleaching is per pixel column (the user's
//                choice, 2026-10-30): the pixel's light x the survival of the
//                column under it, each height at its own excitation (HILO's
//                tilted sheet, TIRF's depth) and the column's past light (the
//                light history's dose at the pixel, as seconds of the light
//                now, as the fiducial beads); the defocus blur of the bleached
//                pattern is left out. Deterministic: the dyes bleach as their
//                mean, at (kEm / kEm at 1 kW/cm^2 at the dye's excitation peak)
//                x ln 2 / slide-bleach-half-sec (as the beads).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Excitation.h"
#include "LightPath.h"

#include <memory>
#include <string>
#include <vector>

namespace sim {

// The slide's dyes: Mix, then the library's (DyeIds()).
const std::vector<std::string>& SlideDyeNames();
// The heights a column's survival is read at (HILO and TIRF; epi's excitation is the same at every height: one).
constexpr int kSlideNodes = 8;
// The collected map's node grid (Excitation::ImagerMap): nodes per beam scale, bilinear within ~0.1 % of the peak (the
// background's 3 left ~1.4 % kinks, visible in a profile of the slide, whose whole image it is).
constexpr double kSlideNodesPerScale = 12;
// The bleach half-time is given at this intensity, kW/cm^2, at the dye's excitation peak.
constexpr double kSlideBleachRefKWcm2 = 1.0;

struct SlideOptions
{
   int dye = 0;                  // SlideDyeNames() index
   double thicknessUm = 5, concMicroM = 10, halfSec = 900;
};
// One dye of the slide through a light path, at excitation 1.
struct SlideComponent
{
   std::string id;
   double perPxUmSec = 0;        // detected photons per pixel per um of layer per second
   double bleachPerSec = 0;      // the bleach hazard (0: does not bleach)
   double lambdaNm = 0;
   ExcitationShares lineShares;
};
// The slide's dyes through lp at camera pixels of pixelUm (left out: those no laser excites or the path does not detect).
// False (with err) on a bad dye.
bool SlidePhotophysics(const SlideOptions& o, const LightPath& lp, double pixelUm, std::vector<SlideComponent>& out,
                       std::string& err);
// The slide's light preset (data/dyes/light_path.json Slide-*): Mix: Slide-561; a library dye: the Slide preset with the
// filters of the dye's WideField preset (its default mode's when it has none), else Slide-561.
std::string SlideLightPresetId(int dye);

// The slide of a movie (FluorescenceMovie::Begin): per component its unbleached photons per pixel per frame, and per
// excitation geometry (components with the same line shares share one) each pixel's excitation at kSlideNodes heights
// (or one), its emission weights, and each pixel's past light as seconds of the light now (t0).
struct SlideMovie
{
   struct Geometry
   {
      ExcitationShares shares;
      int nodes = 1;
      std::vector<float> a;      // nodes x W x H: the excitation at each height (0: dark)
   };
   struct Comp
   {
      SlideComponent c;
      size_t geo = 0;
      std::vector<float> base;   // photons per pixel per frame, unbleached
   };
   unsigned W = 0, H = 0;
   double expSec = 0.05;
   std::vector<Geometry> geos;
   std::vector<Comp> comps;
   // The past light: none (empty): t0Sec of the movie's own light at every height; under a light history per pixel
   // (LightPast) seconds of the light now (each height at its own excitation) plus seconds at excitation 1 (every
   // height alike), then the frames from t0Sec on at each height's own excitation.
   std::vector<float> pastSec, pastExc;
   double t0Sec = 0;
   double sec = 0;               // render time, s
};
// A place's past light under a host's light history (LightHistory::DosesAt: its dose there from the light now's
// chapter and from the others, the dose rate the light now gives there and at its beam's peak; the light now's F), as
// seconds of the light now (sec: x the excitation at any height) plus seconds at excitation 1 (exc: every height
// alike). The light now's chapter (its geometry: its heights scale as the light now's) as seconds of the light now
// where it reaches the place (rate >= 1 % of the peak's: exact under an unchanged light, the history lattice's
// interpolation cancels), as excitation where it does not (< 0.1 %: dose / the peak's rate x F), a smooth blend
// between (they differ by the lattice's ~0.2 %); the other chapters as excitation (their heights unknown here: the
// column alike, the per-column model). Seconds of the light now alone (dose / rate) jumped from huge to 0 at the
// footprint's 1e-4 cut where the light now barely reaches (HILO focused high: its sheet meets the coverslip tens of um
// aside; a step across the image, the user's report, 2026-10-30), and gave an earlier light of another geometry the
// light now's heights (epi's past under HILO: over-bleached where the sheet is bright).
struct LightPast { double sec = 0, exc = 0; };
LightPast LightPastAt(double doseNow, double doseOther, double rate, double peakRate, double F);
// The slide's photons of frame f into img: each pixel's base x the mean survival of its column over the frame.
void RenderSlide(std::vector<float>& img, const SlideMovie& M, long f);

} // namespace sim
