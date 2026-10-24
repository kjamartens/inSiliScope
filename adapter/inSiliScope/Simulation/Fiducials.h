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
//                plane at its nearest sub-sample (one tap) out to the point
//                kernel's window, and beyond that, with a bead halo (BeadHalo:
//                a window twice the FOV, one sample per pixel, one tap), the
//                halo's samples out to where the bead's light ends (Extent);
//                with a halo the part in the point kernel's window is scaled
//                to the halo's share there. Bead kernels are built per (point
//                kernel, plane, diameter on a kBeadBinNm grid) and shared
//                process-wide.
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

// The beads' halo: the point PSF over a window twice the FOV (half-width the FOV's larger side, at least the point
// kernel's, at most kFiducialHaloMaxUm), one sample per camera pixel (so its planes are their own block sums), on the
// point kernel's z planes: a bright or defocused bead lights the whole frame, where the point kernel's window (a few um
// in the viewer) would cut its light off square.
// A splat's halo ends where every sample beyond it gives fewer than kFiducialHaloMinPhotons per pixel (Extent), so a
// dim bead costs little and a bright or defocused one reaches as far as its light shows.
constexpr double kFiducialHaloMaxUm = 30, kFiducialHaloMinPhotons = 0.1;
class BeadHalo
{
public:
   explicit BeadHalo(const PsfKernelCache& wide);   // keeps its geometry and block sums
   const PsfKernelCache& Geom() const { return geom_; }
   const std::vector<float>& Plane(int k) const { return sums_[static_cast<size_t>(k)]; }
   int Nz() const { return geom_.nz; }
   uint64_t Id() const { return id_; }
   // The camera half-width a splat of `photons` at plane k needs: beyond it every sample x |photons| is below
   // kFiducialHaloMinPhotons (+ 1 for the interpolation's second tap), at most the window's.
   int Extent(int k, double photons) const;

private:
   PsfKernelCache geom_;   // the geometry only (no data)
   std::vector<std::vector<float>> sums_;
   std::vector<std::vector<float>> beyond_;   // per plane and Chebyshev radius r: the largest sample at r or beyond
   uint64_t id_;
};
// The halo of a request (oversampling 1, its half-width set by the caller; GibsonLanniZernike; memoized, a few
// entries apart from the point kernels' memo, so the beads do not push the dyes' kernels out). False with err.
bool BeadHaloFor(const PsfGeneratorRequest& req, std::shared_ptr<const BeadHalo>& out, std::string& err);
// The halo's camera half-width for a W x H frame of pixelNm and a point kernel of camRad camera pixels (JS
// beadHaloHalfWidthPx).
int BeadHaloHalfWidthPx(unsigned W, unsigned H, double pixelNm, int camRad);

// The bead kernels of one point kernel at one camera pixel size.
class BeadKernel
{
public:
   BeadKernel(const PsfKernelCache& K, double pixelNm);
   const PsfKernelCache& Point() const { return K_; }
   // Splat one bead of diameter dNm with `photons` (any sign: a Taylor image's odd orders are negative) at (xPx, yPx)
   // (camera pixels) and plane k into img's rows [rowLo, rowHi) (rowHi < 0: H), its halo from halo when given (on the
   // same z planes) (JS splatBead, which takes every row).
   void Splat(std::vector<float>& img, unsigned W, unsigned H, int k, double dNm, double xPx, double yPx,
              double photons, const BeadHalo* halo = nullptr, int rowLo = 0, int rowHi = -1);

private:
   struct Core
   {
      PsfKernelCache geom;   // oversampling, size, half-width, block-sum width (SplatSetup's inputs)
      std::vector<float> blockSums;
   };
   const Core& CoreAt(int k, double dBin);
   // With a halo, the point kernel's part ends at this camera radius: its window but the outermost ring, whose pixels
   // reach half a pixel beyond its samples (at least the core's).
   int InnerRadius() const { return std::max(rc_, K_.halfWidthOversampled / std::max(1, K_.oversampling) - 1); }
   // The halo's photons in the point kernel's part (InnerRadius) over the point kernel's, at plane k (one emitter at a
   // pixel centre).
   double InnerScale(const BeadHalo& halo, int k);
   std::vector<float> Plane(int k, double dNm) const;
   PsfKernelCache K_;
   double pixelNm_;
   int rc_ = 0, h_ = 0;
   PsfInterpMode interp_;
   std::mutex mu_;
   std::map<long long, std::unique_ptr<Core>> cores_;
   std::map<uint64_t, std::vector<double>> scales_;   // per halo id, per plane (NaN: not yet)
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
   double halfSec = 288, refKWcm2 = 1, blockingOD = 8;
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
   std::shared_ptr<const BeadHalo> halo;  // the wide halo (with a kernel)
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
