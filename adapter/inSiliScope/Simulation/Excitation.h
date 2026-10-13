///////////////////////////////////////////////////////////////////////////////
// FILE:          Excitation.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Excitation geometry (2026-10-13): the beam's in-plane profile
//                and where the laser spot sits in the objective's back focal
//                plane -- epi, HILO, TIRF -- as one 3D excitation factor. The
//                twin of web/prototype/scope/excitation.js (its header has the
//                physics and the references; spec/ALGORITHM.md "Excitation
//                geometry").
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

   int Regime() const { return regime_; }
   double U() const { return u_; }
   double UMax() const { return uMax_; }
   bool Clipped() const { return clipped_; }
   bool Tirf() const { return tirf_; }
   double F() const { return F_; }
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
   // TIRF's 1/e depth at lambdaNm; HILO's sheet thickness on the axis; -1 otherwise (nm).
   double DepthNm(double lambdaNm = 640) const;
   // TIRF's decay length at lambdaNm, um.
   double DecayUm(double lambdaNm) const;

private:
   int profile_ = ILLUM_FLATTOP, regime_ = REGIME_EPI;
   double u_ = 0, uMax_ = 0, F_ = 1, cosI_ = 1, tanS_ = 0, nS_ = 1.337, zp_ = 0;
   double fwhmUm_ = 1, sig_ = 1, r0_ = 1, edge_ = 1, flatTop0_ = 1, reachUm_ = 0;
   bool clipped_ = false, tirf_ = false, uniform_ = false;
};

// Dose levels of a population whose photons per dye are nonlinear in its excitation factor a (JS doseNodes,
// hatWeight): a dye at factor a is split over the two neighbouring nodes by linear interpolation; one image per node.
constexpr double kDoseStep = 0.5, kDoseCut = 8.0;
constexpr int kDoseLevels = 16;
std::vector<double> DoseNodes(double aMax, double dMax);
double HatWeight(const std::vector<double>& nodes, size_t k, double a);

} // namespace sim
