///////////////////////////////////////////////////////////////////////////////
// FILE:          Fiducials.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Fiducial beads: their light and image. Where the beads are is
//                the core's (core/src/fiducials.*, isc_fiducials_in_window);
//                this is what a bead looks like and how bright it is.
//
//                A bead is one emitter with the PSF of a uniformly bright solid
//                sphere, never its ~1e4-1e6 dyes: one splat per bead and
//                component per frame. Its image has two parts around its centre
//                pixel: the core (|dx|, |dy| <= rc = ceil(kBeadCoreUm / pixel)
//                pixels), the bead kernel -- the point PSF averaged over the
//                sphere (in z over its slabs, kBeadSlabNm apart, each the PSF
//                linearly interpolated between planes and weighted by the slab's
//                disc area; then laterally over its projection, column density
//                ~ sqrt(r^2 - rho^2)), faded into the point PSF over the core's
//                outer kBeadFadeUm, read with the movie's interpolation (Fft:
//                Cubic) --, and the halo beyond it: the point PSF of the nearest
//                plane at its nearest sub-sample (one tap). Bead kernels are
//                built per (point kernel, plane, diameter on a kBeadBinNm grid)
//                and shared process-wide.
//
//                Light per component (FiducialPhotophysics): an equivalent dye
//                (data/fiducials, balinovic2019) through the light path, a bead
//                of diameter d holds emitters x (d / refDiameterNm)^3 of them;
//                bleaching deterministic (the dyes bleach as their mean) at
//                (kEm / kEm at the reference) x ln 2 / halfSec; gold also
//                scatters each laser line (a Mie table) through the emission
//                path, floored at 10^-blockingOD.
//
//                JS twin: web/prototype/scope/fiducials.js (the bead, the light)
//                and fluorescence.js (fiducialBeads, renderFiducials).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Excitation.h"
#include "LightPath.h"
#include "PsfGeneratorBridge.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sim {

constexpr double kBeadCoreUm = 1.0, kBeadFadeUm = 0.4, kBeadSlabNm = 10, kBeadBinNm = 10;
constexpr int kBeadLateralSub = 4;
// A component whose brightest bead gives fewer detected photons per frame is left out (no kernel, no splat).
constexpr double kFiducialMinPhotons = 1e-3;
// A bead component takes a dye group's kernel within this of its wavelength, else a kernel of its own.
constexpr double kFiducialKernelReuseNm = 10;
// The static (no drift) images: a Taylor series of each bead's decay about the movie's middle, of the lowest order
// whose remainder is below kFiducialTaylorTol, at most kFiducialTaylorMaxOrder (else splats per frame).
constexpr double kFiducialTaylorTol = 1e-9;
constexpr int kFiducialTaylorMaxOrder = 6;

// The diameter bin of a bead kernel (JS beadBinNm).
double BeadBinNm(double dNm);

// The bead kernels of one point kernel at one camera pixel size.
class BeadKernel
{
public:
   BeadKernel(const PsfKernelCache& K, double pixelNm);
   const PsfKernelCache& Point() const { return K_; }
   // Splat one bead of diameter dNm with `photons` (any sign: a Taylor image's odd orders are negative) at (xPx, yPx)
   // (camera pixels) and plane k into img (JS splatBead).
   void Splat(std::vector<float>& img, unsigned W, unsigned H, int k, double dNm, double xPx, double yPx,
              double photons);

private:
   struct Core
   {
      PsfKernelCache geom;   // oversampling, size, half-width, block-sum width (SplatSetup's inputs)
      std::vector<float> blockSums;
   };
   const Core& CoreAt(int k, double dBin);
   std::vector<float> Plane(int k, double dNm) const;
   PsfKernelCache K_;
   double pixelNm_;
   int rc_ = 0, h_ = 0;
   PsfInterpMode interp_;
   std::mutex mu_;
   std::map<long long, std::unique_ptr<Core>> cores_;
};
// The bead kernels of K at pixelNm (memoized with the kernel's planes and interpolation; JS beadKernel).
std::shared_ptr<BeadKernel> BeadKernelFor(const PsfKernelCache& K, double pixelNm);

// Gold's elastic scattering cross section at a wavelength and diameter, um^2 (the Mie table, bilinear, clamped to it;
// below its smallest sphere ~ d^6).
double GoldScatteringUm2(double lambdaNm, double dNm);

// One component of a bead type through a light path, at excitation 1 and per equivalent dye.
struct FiducialComponent
{
   std::string id, name;
   bool scatter = false;          // gold's elastic scatter of one laser line
   double lambdaNm = 0;           // its PSF wavelength
   double detectedPerSec = 0, detectedFraction = 0;
   ExcitationShares lineShares;
   double bleachPerSec = 0;       // the bleach hazard at excitation 1 (0: does not bleach)
   double emitters = 0, refDiameterNm = 100;
   double perUm2 = 0;             // scatter: detected photons per second per um^2 of cross section
};
struct FiducialLightOptions
{
   double dyes = -1;              // > 0: every component's equivalent dye count at the reference diameter
   double halfSec = 28800, refKWcm2 = 1, blockingOD = 8;
};
// Bead type t (FiducialTypeIds() index) through lp (JS fiducialPhotophysics). False (with err) on a bad type.
bool FiducialPhotophysics(int t, const LightPath& lp, const FiducialLightOptions& o, std::vector<FiducialComponent>& out,
                          std::string& err);
// A component's detected photons per second from one bead of diameter dNm at excitation 1 (before bleaching).
double BeadRatePerSec(const FiducialComponent& c, double dNm);

// The beads of a movie (FluorescenceMovie: JS fiducialBeads's result): per component its beads in the FOV frame with
// their rate (detected photons per second at their excitation, before bleaching), bleach rate and the time their decay
// starts at (the movie's start, or a host's dose), the kernel.
struct FiducialBeadSite
{
   double xUm, yUm, zNm, d;
   double rate, lambda, t0Sec;
};
struct FiducialMovieComponent
{
   FiducialComponent c;
   std::vector<FiducialBeadSite> beads;
   double lambdaNm = 0, sigmaPx = 1;
   PsfKernelCache kernel;                 // !valid: the Gaussian of sigmaPx (+ the sphere)
   std::shared_ptr<BeadKernel> B;
   int taylorState = -1;                  // -1 not built, 0 per frame, 1 the images
   double taylorZ = 0;                    // the focus they were built at
   std::vector<std::vector<float>> taylor;
};
struct FiducialMovie
{
   int type = 0;
   size_t sites = 0;
   std::vector<FiducialMovieComponent> comps;
   double expSec = 0.05;
   long N = 0;
   double sec = 0;
};
// The beads' photons of frame f into img (JS renderFiducials): per component, each bead's rate x the integral of its
// survival over the frame. staticFocus: no drift and the focus zf the same in every frame: through the Taylor images
// (built at the first frame), else splatted one by one at the drift (dx, dy) px.
void RenderFiducials(std::vector<float>& img, unsigned W, unsigned H, FiducialMovie& FM, long f, double zf,
                     double pixelNm, bool staticFocus, double dx, double dy);

} // namespace sim
