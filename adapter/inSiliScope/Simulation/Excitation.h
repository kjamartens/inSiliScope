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
//                Several beams (2026-10-30, SimPattern.h, spec/ALGORITHM.md
//                "Structured illumination"): a spec with a beam set makes one
//                component per beam -- the single beam's physics at the beam's
//                own position (azimuth, angle, F by its polarization), its own
//                envelope (its spot size or the legacy width, stretched along
//                its azimuth, centred by its tilt) and depth -- and the
//                frame's pattern |sum_j E_j|^2 per laser line (CompilePattern):
//                At = the pattern (the emission), MeanAt = the components' sum
//                (the cycle mean: the incoherent sum, which the light history
//                records and the kinetics run on). Without a beam set the one
//                legacy component is the old arithmetic, bit for bit.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "SimPattern.h"

#include <cmath>
#include <complex>
#include <cstdint>
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
   // Several beams (SimPattern.h): empty beams = the legacy single beam at (bfp, 0) above.
   BeamSet beams;
   // Where the beams are in phase (sim-anchor, 2026-10-31): true = the focal plane (pivotUm): the fringes move with
   // the focus and the sample moves through them (the stage or the objective: the same relative motion), as a
   // grating or SLM conjugate to the focal plane makes them; false = the coverslip: the fringes fixed to the sample.
   bool anchorFocus = true;
   double fObjUm = 2000;         // the objective's focal length (tube length / magnification): the pupil radius is f NA,
                                 // a spot of radius w_b makes an envelope of 1/e^2 radius lambda f / (pi w_b), a tilt
                                 // moves it by f tan(tilt)
   double lambdaRefNm = 640;     // the line the envelope widths are made at (a spot size: the strongest line at the sample)
   std::vector<double> lineNm;   // the lines the pattern is compiled for (the lasers); empty: lambdaRefNm alone
   uint32_t seed = 0;            // the phase drift's seed
   double expSec = 0.05;         // ... and frame time
   long long frame = 0;          // the frame whose pattern this is (Excitation::SetFrame changes it)
};

class Excitation
{
public:
   Excitation() { Make(ExcitationSpec()); }
   explicit Excitation(const ExcitationSpec& s) { Make(s); }
   void Make(const ExcitationSpec& s);
   const ExcitationSpec& Spec() const { return spec_; }

   // A beam set given (At is the frame's pattern; MeanAt the cycle mean); else the legacy single beam (At = MeanAt).
   bool Patterned() const { return patterned_; }
   // The pattern changes with the frame (a program of several frames, or a phase drift).
   bool Varies() const { return patterned_ && spec_.beams.Varies(); }
   // The frame whose pattern At gives (Patterned; the program's state and the phase drift at it); cheap (the terms
   // per line are recompiled, no grids).
   void SetFrame(long long frame);
   long long Frame() const { return spec_.frame; }
   const PatternState& State() const { return state_; }
   // The pattern of a line at the current frame (Patterned; nullptr for a line it was not compiled for).
   const Pattern* PatternOf(double nm) const;
   // At averaged over a ball of radius rUm about the point (Pattern::BallMean, the envelopes at the centre), and
   // the frame's beams' incoherent part over the same ball: a fiducial bead's light in the frame is the ratio x
   // its mean (a bead wider than a fringe is never dark). One beam: At.
   double AtBall(double x, double y, double z, double rUm, const ExcitationShares& shares) const;
   double DcAtBall(double x, double y, double z, double rUm, const ExcitationShares& shares) const;
   // The lateral fringe periods at nm, um (largest first; empty without fringes).
   std::vector<double> PeriodsUm(double nm) const;

   // The outermost beam's regime / u; F = the cycle mean's value at the axis on the focal plane (one beam: its F).
   int Regime() const { return regime_; }
   double U() const { return u_; }
   double UMax() const { return uMax_; }
   bool Clipped() const { return clipped_; }
   bool Tirf() const { return tirf_; }
   // Some beam crosses the sample at an angle (HILO): its footprint moves with the height, about the pivot.
   bool Tilted() const { return tilted_; }
   double F() const { return F_; }
   double CosI() const { return cosI_; }
   double TanS() const { return tanS_; }
   // How far from the axis the profile stays at or above frac of its peak (um; x' as ProfileAt's, so x reaches
   // ReachUm / CosI); infinite for a Flat beam. Several beams: the largest, from the envelopes' centres.
   double ReachUm(double frac) const;
   // The profile's largest second derivative, |d^2 P / dx'^2| (1/um^2, peak 1; 0 for a Flat beam): a footprint
   // sampled every h um is within h^2 / 8 of this of the peak between samples (bilinear). Several beams: the largest.
   double MaxCurvature() const;
   // Exactly 1 everywhere (a Flat beam at the BFP centre): the renderers skip the factor (outputs as before).
   bool Uniform() const { return uniform_; }

   // One beam's component: its geometry, factor, envelope and depth (ComponentCount = the beams; 1 for the legacy
   // beam). The light history records one grid per component (its footprint: Lateral on the pivot plane) with the
   // component's shear (sx, sy: tan theta_s along its azimuth), F and depth table.
   int ComponentCount() const { return static_cast<int>(comps_.size()); }
   struct Component
   {
      double u = 0, cosI = 1, tanS = 0, F = 1;
      double sx = 0, sy = 0;        // the footprint's shear per um of height (HILO), 0 in TIRF / epi
      double ox = 0, oy = 0;        // the envelope's centre (the beam's tilt), um from the axis
      double azimuth = 0;           // rad
      double reachUm = 0, curvature = 0;
      bool tirf = false, clipped = false, flat = false;
      double decayScale = 0;        // DecayUm(nm) = decayScale x nm (TIRF), 0 otherwise
      int beam = -1;                // its beam in Spec().beams (-1: the legacy beam)
   };
   const Component& ComponentAt(int j) const { return comps_[static_cast<size_t>(j)]; }
   // Component j's footprint on the plane at height z (its envelope, no field factor), and its depth factors.
   double ComponentLateral(int j, double x, double y, double z) const;
   double ComponentDepth(int j, double z, const ExcitationShares& shares) const;
   double ComponentDepthMean(int j, double z0, double z1, const ExcitationShares& shares) const;
   double ComponentDecayUm(int j, double lambdaNm) const;

   // The in-plane profile at (x', y) um from the axis, peak 1 (the legacy beam's: the first component's).
   double ProfileAt(double xp, double y) const;
   // The footprint on the plane at height z (no field factor): the legacy beam's (the first component's).
   double Lateral(double x, double y, double z) const;
   // The depth factor (TIRF: the shares' exponentials; else 1); z < 0 counts as 0 (the first component's).
   double Depth(double z, const ExcitationShares& shares) const;
   // Its mean over [z0, z1] (a WideField plane), exact.
   double DepthMean(double z0, double z1, const ExcitationShares& shares) const;
   // The frame's excitation: F x Lateral x Depth (0 when clipped) for the legacy beam; the pattern |sum E_j|^2 x
   // envelopes for a beam set (the shares' lines mixed by their shares).
   double At(double x, double y, double z, const ExcitationShares& shares) const;
   // The cycle mean: sum over components of F_j x Lateral_j x Depth_j (= At for the legacy beam, bit for bit).
   double MeanAt(double x, double y, double z, const ExcitationShares& shares) const;
   // The exact mean of At over the plane [z0, z1] at (x, y), and over a cell of cellUm around (x, y) laterally
   // (cellUm 0: at the point; the fringes' means are analytic: sinc over the cell, the damped cosine over the plane;
   // the envelopes at the plane's centre); MeanAt's the same (the mean field's channel 0: F x Lateral x DepthMean).
   double PlaneMean(double x, double y, double z0, double z1, double cellUm, const ExcitationShares& shares) const;
   double MeanPlaneMean(double x, double y, double z0, double z1, const ExcitationShares& shares) const;
   // PlaneMean at many cells of one plane (2026-11-11): the parts the plane alone sets (the damped cosines' integrals,
   // the sincs, the beams' rotations, the lines' patterns) made once by PreparePlaneMean, the rest per cell by
   // PlaneMeanAt -- the same operations on the same values, so PlaneMeanAt = PlaneMean bit for bit. Patterned and not
   // clipped only (prep.ok false: call PlaneMean).
   struct PlaneMeanPrep
   {
      struct Comp
      {
         bool clipped = false, plain = false;   // plain: no azimuth, no offset (ComponentLateral's first branch)
         const void* env = nullptr;             // its Envelope
         double ox = 0, oy = 0, ca = 1, sa = 0, shift = 0, cosI = 1;
      };
      struct Term
      {
         size_t j = 0, l = 0;
         bool same = false;
         double amp = 0, dc = 1;                // same: the decay's mean (or 1)
         double qx = 0, qy = 0, phase = 0;      // a fringe: theta = qx x + qy y + phase
         double ea = 0, kza = 0;                // a fringe on a flat plane: exp(sr a) cos(theta + kz a)
         std::complex<double> integral;         // a fringe on a plane: the integral of exp(s z) over [a, b]
         bool sinc = false;
         double sincs = 1;                      // the cell's two sincs' product
      };
      struct Line
      {
         double share = 0;
         std::vector<Term> terms;
      };
      bool ok = false, flat = false;            // flat: !(b > a)
      double ba = 0;                            // b - a
      std::vector<Comp> comps;
      std::vector<Line> lines;
      mutable std::vector<double> env;          // per cell scratch
   };
   void PreparePlaneMean(double z0, double z1, double cellUm, const ExcitationShares& shares, PlaneMeanPrep& p) const;
   double PlaneMeanAt(double x, double y, const PlaneMeanPrep& p) const;
   // The column integral of MeanAt over the chamber [0, H] um at (x, y) (the free imager's excitation).
   double Column(double x, double y, double H, const ExcitationShares& shares) const;
   // The free imagers' collected column at (x, y) (JS imagerColumnAt): every height's lit imager defocused onto the
   // pixel, sum of rate x um; at a chosen accuracy (the checks' dense reference). On the cycle mean.
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
   // TIRF's 1/e depth at lambdaNm; HILO's sheet thickness on the axis; -1 otherwise (nm). The outermost beam's.
   double DepthNm(double lambdaNm = 640) const;
   // TIRF's decay length at lambdaNm, um (the outermost beam's).
   double DecayUm(double lambdaNm) const;
   // The equivalent spot of the legacy width (or a beam's): the Gaussian spot's 1/e^2 radius in the pupil, um, of an
   // envelope of FWHM fwhmUm at lambdaRefNm; and back.
   double SpotRadiusUm(double fwhmUm) const;
   double EnvelopeFwhmUm(double spotUm, double lambdaNm) const;

private:
   struct Interval { double zm, w; };
   std::vector<Interval> Intervals(double Hc, const std::vector<ImagerSource>& imagers, double ratio, int nTirf) const;
   double CollectedAt(double x, double y, const std::vector<Interval>& ivs, const CollectionRings& rings,
                      const std::vector<double>& dirs, double small) const;
   struct Envelope
   {
      double fwhmUm = 1, sig = 1, r0 = 1, edge = 1, flatTop0 = 1;
   };
   double EnvelopeAt(const Envelope& e, double xp, double y) const;
   void MakeLegacy(const ExcitationSpec& s);
   void MakePatterned(const ExcitationSpec& s);
   void Compile();

   ExcitationSpec spec_;
   int profile_ = ILLUM_FLATTOP, regime_ = REGIME_EPI;
   double u_ = 0, uMax_ = 0, F_ = 1, cosI_ = 1, tanS_ = 0, nS_ = 1.337, nI_ = 1.518, na_ = 1.4, zp_ = 0;
   double fwhmUm_ = 1, sig_ = 1, r0_ = 1, edge_ = 1, flatTop0_ = 1, reachUm_ = 0;
   bool clipped_ = false, tirf_ = false, uniform_ = false, tilted_ = false, patterned_ = false;
   // The frame's beam envelopes at a point (Patterned): env[j] for the state's beam j.
   void FrameEnvelopes(double x, double y, double z, std::vector<double>& env) const;
   std::vector<Component> comps_;
   std::vector<Envelope> envs_;   // per component
   PatternState state_;
   std::vector<std::pair<double, Pattern>> patterns_;   // per line (nm)
};

} // namespace sim
