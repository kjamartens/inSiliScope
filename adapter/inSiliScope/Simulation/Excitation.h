///////////////////////////////////////////////////////////////////////////////
// FILE:          Excitation.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Excitation geometry (2026-10-13): the beam's in-plane profile
//                and where the laser spot sits in the objective's back focal
//                plane -- epi, HILO, TIRF -- as one 3D excitation factor
//                (the physics and the references: spec/ALGORITHM.md
//                "Excitation geometry").
//
//                u (option illum-bfp) = the spot's distance from the BFP centre
//                in units of the critical angle's radius: n_i sin(theta_i) =
//                u n_s; 0 epi, 1 the critical angle, > 1 TIRF up to the pupil
//                edge NA / n_s, beyond it clipped (no light). The factor F is
//                |E|^2 in the sample over its epi value (Fresnel below the
//                critical angle, Axelrod's evanescent intensities above,
//                decaying as exp(-z / d)); the beam lands on a plane at height
//                z as P((x - (z - z_p) tan theta_s) cos theta_i, y).
//                Coordinates: um from the optical axis in x, y; z = height
//                above the coverslip, um.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace sim {

enum ExcitationProfile { ILLUM_FLAT = 0, ILLUM_GAUSSIAN = 1, ILLUM_FLATTOP = 2 };   // = SMLMBackground.h IllumProfile
const std::vector<std::string>& IlluminationProfileNames();   // Flat, Gaussian, FlatTop
const std::vector<std::string>& IlluminationRegimeNames();    // Epi, HILO, TIRF, Clipped
enum IllumRegime { REGIME_EPI = 0, REGIME_HILO = 1, REGIME_TIRF = 2, REGIME_CLIPPED = 3 };

// A dye state's excitation per laser line ({nm, share}, the shares summing to 1; StatePhysics::lineShares).
struct ExcitationLine { double nm, share; };
using ExcitationShares = std::vector<ExcitationLine>;

// A free imager (a DNA-PAINT label's): its background per pixel per um of lit height per second and its line shares.
struct ImagerSource { double rate; ExcitationShares shares; };

// The collection of a defocused dye's light (JS collectionRings; its comment has the model and references): the
// emission angles' quantiles, as tan theta in the medium, and their weights (summing to 1). n = 0: 16-point
// Gauss-Legendre in the quantile (the renderers); n > 0: n equal-weight midpoints. fresnel false: isotropic.
struct CollectionRings { std::vector<double> tans, weights; };
CollectionRings CollectionRingsFor(double nS, double nI, double na, int n, bool fresnel = true);
constexpr int kCollectDirs = 24, kCollectTirfSteps = 64, kCollectNodes = 64;
constexpr double kCollectRatio = 1.15, kCollectDz0 = 0.05;

struct ExcitationSpec
{
   int profile = ILLUM_FLATTOP;
   double fwhmPct = 100, fovWidthUm = 12.8;
   double bfp = 0;               // u
   double na = 1.4, nImm = 1.518, nMed = 1.337;
   double pivotUm = 0;           // where the HILO beam crosses the axis (the focal plane)
};

class Excitation
{
public:
   Excitation() { Make(ExcitationSpec()); }
   explicit Excitation(const ExcitationSpec& s) { Make(s); }
   void Make(const ExcitationSpec& s);
   const ExcitationSpec& Spec() const { return spec_; }

   int Regime() const { return regime_; }
   double U() const { return u_; }
   double UMax() const { return uMax_; }
   bool Clipped() const { return clipped_; }
   bool Tirf() const { return tirf_; }
   // The beam crosses the sample at an angle (HILO): its footprint moves with the height, about the pivot.
   bool Tilted() const { return !tirf_ && tanS_ != 0.0; }
   double F() const { return F_; }
   double CosI() const { return cosI_; }
   double TanS() const { return tanS_; }
   // How far from the axis the profile stays at or above frac of its peak (um; x' as ProfileAt's, so x reaches
   // ReachUm / CosI); infinite for a Flat beam.
   double ReachUm(double frac) const;
   // The profile's largest second derivative, |d^2 P / dx'^2| (1/um^2, peak 1; 0 for a Flat beam): a footprint
   // sampled every h um is within h^2 / 8 of this of the peak between samples (bilinear).
   double MaxCurvature() const;
   // Exactly 1 everywhere (a Flat beam at the BFP centre): the renderers skip the factor (outputs as before).
   bool Uniform() const { return uniform_; }

   // The in-plane profile at (x', y) um from the axis, peak 1.
   double ProfileAt(double xp, double y) const;
   // The footprint on the plane at height z (no field factor).
   double Lateral(double x, double y, double z) const;
   // The depth factor (TIRF: the shares' exponentials; else 1); z < 0 counts as 0.
   double Depth(double z, const ExcitationShares& shares) const;
   // Its mean over [z0, z1] (a WideField plane), exact.
   double DepthMean(double z0, double z1, const ExcitationShares& shares) const;
   // F x Lateral x Depth (0 when clipped).
   double At(double x, double y, double z, const ExcitationShares& shares) const;
   // The column integral of At over the chamber [0, H] um at (x, y) (the free imager's excitation).
   double Column(double x, double y, double H, const ExcitationShares& shares) const;
   // The free imagers' collected column at (x, y) (JS imagerColumnAt): every height's lit imager defocused onto the
   // pixel, sum of rate x um; at a chosen accuracy (the checks' dense reference).
   double ImagerColumnAt(double x, double y, double Hc, const std::vector<ImagerSource>& imagers, int rings = 0,
                         int dirs = kCollectDirs, double ratio = kCollectRatio, int tirfSteps = kCollectTirfSteps,
                         bool fresnel = true) const;
   // Its map over a W x H camera (pixel (i, j) at (i um - axisX, j um - axisY); JS imagerMap): on a node grid,
   // interpolated bilinearly; a Flat beam: the plain columns. Nodes on threads (each its own slot). The grid: nodesPerScale
   // nodes per beam scale (the Gaussian's sigma, the FlatTop's edge), at most one per pixel and kCollectNodes per axis;
   // bilinear between them is within ~1 / (8 nodesPerScale^2) of the peak (3: ~1.4 %, the background's; 12: ~0.1 %,
   // the fluorescent slide's, whose whole image this is).
   std::vector<double> ImagerMap(unsigned W, unsigned H, double um, double axisX, double axisY, double Hc,
                                 const std::vector<ImagerSource>& imagers, double nodesPerScale = 3.0) const;
   // TIRF's 1/e depth at lambdaNm; HILO's sheet thickness on the axis; -1 otherwise (nm).
   double DepthNm(double lambdaNm = 640) const;
   // TIRF's decay length at lambdaNm, um.
   double DecayUm(double lambdaNm) const;

private:
   struct Interval { double zm, w; };
   std::vector<Interval> Intervals(double Hc, const std::vector<ImagerSource>& imagers, double ratio, int nTirf) const;
   double CollectedAt(double x, double y, const std::vector<Interval>& ivs, const CollectionRings& rings,
                      const std::vector<double>& dirs, double small) const;

   ExcitationSpec spec_;
   int profile_ = ILLUM_FLATTOP, regime_ = REGIME_EPI;
   double u_ = 0, uMax_ = 0, F_ = 1, cosI_ = 1, tanS_ = 0, nS_ = 1.337, nI_ = 1.518, na_ = 1.4, zp_ = 0;
   double fwhmUm_ = 1, sig_ = 1, r0_ = 1, edge_ = 1, flatTop0_ = 1, reachUm_ = 0;
   bool clipped_ = false, tirf_ = false, uniform_ = false;
};

} // namespace sim
