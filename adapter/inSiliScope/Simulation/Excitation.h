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
   // The free imagers' collected column at (x, y) (JS imagerColumnAt): every height's lit imager defocused onto the
   // pixel, sum of rate x um; at a chosen accuracy (the checks' dense reference).
   double ImagerColumnAt(double x, double y, double Hc, const std::vector<ImagerSource>& imagers, int rings = 0,
                         int dirs = kCollectDirs, double ratio = kCollectRatio, int tirfSteps = kCollectTirfSteps,
                         bool fresnel = true) const;
   // Its map over a W x H camera (pixel (i, j) at (i um - axisX, j um - axisY); JS imagerMap): on a node grid,
   // interpolated bilinearly; a Flat beam: the plain columns. Nodes on threads (each its own slot).
   std::vector<double> ImagerMap(unsigned W, unsigned H, double um, double axisX, double axisY, double Hc,
                                 const std::vector<ImagerSource>& imagers) const;
   // TIRF's 1/e depth at lambdaNm; HILO's sheet thickness on the axis; -1 otherwise (nm).
   double DepthNm(double lambdaNm = 640) const;
   // TIRF's decay length at lambdaNm, um.
   double DecayUm(double lambdaNm) const;

private:
   struct Interval { double zm, w; };
   std::vector<Interval> Intervals(double Hc, const std::vector<ImagerSource>& imagers, double ratio, int nTirf) const;
   double CollectedAt(double x, double y, const std::vector<Interval>& ivs, const CollectionRings& rings,
                      const std::vector<double>& dirs, double small) const;

   int profile_ = ILLUM_FLATTOP, regime_ = REGIME_EPI;
   double u_ = 0, uMax_ = 0, F_ = 1, cosI_ = 1, tanS_ = 0, nS_ = 1.337, nI_ = 1.518, na_ = 1.4, zp_ = 0;
   double fwhmUm_ = 1, sig_ = 1, r0_ = 1, edge_ = 1, flatTop0_ = 1, reachUm_ = 0;
   bool clipped_ = false, tirf_ = false, uniform_ = false;
};

// Dose levels of a population whose photons per dye are nonlinear in its excitation factor a (JS doseNodes,
// hatWeight): a dye at factor a is split over the two neighbouring nodes by linear interpolation; one image per node.
constexpr double kDoseStep = 0.5, kDoseCut = 8.0;
constexpr int kDoseLevels = 16;
std::vector<double> DoseNodes(double aMax, double dMax);
double HatWeight(const std::vector<double>& nodes, size_t k, double a);

// The MM adapter's illumination history (IlluminationHistory.h): its tiles and z bands.
constexpr double kHistoryTileUm = 0.25;
// The world z bands of the history (um above the coverslip): band b is [edge b - 1, edge b), the first from -inf, the
// last to +inf. Fine near the coverslip (TIRF's decay, ~0.1-0.2 um), <= 1.5 um above (the HILO sheet moves tan
// theta_s um sideways per um of height).
constexpr int kHistoryBands = 12;
const std::vector<double>& HistoryBandEdges();   // the kHistoryBands - 1 inner edges
// The heights a band's light is evaluated over: band 0 from the coverslip (0), the last up to 8 um (the cells' tops).
void HistoryBandRange(int band, double& z0Um, double& z1Um);

// The light the history counts under an excitation (ScopeExcitation: the geometry and the first label's line shares):
// per band F x the band's mean depth factor in 1/16 steps where the beam's footprint (at the band's middle height:
// the HILO sheet moves with it) is at least half its peak, else 0. The footprint is lit or not, not its soft edge:
// with its 1/16 levels a FlatTop's edge made hundreds of clock regions (each a core query; live frames took
// seconds), lit / unlit makes a few. The photons keep the smooth footprint (Excitation::At); the kinetics read
// this (FluorescenceMovie's dose events), so a frame reads the dose the history gives it.
class HistoryLightField {
public:
   HistoryLightField(const Excitation& ex, const std::vector<ExcitationLine>& shares);
   // The light of band b at dx, dy um from the optical axis (a tile centre).
   double Band(double dxUm, double dyUm, int b) const;
   void Fill(double dxUm, double dyUm, float* w) const;   // every band
   static int BandOf(double zUm);                         // the band a world height is in
   static double TileCentre(double vUm);                  // the centre of the history tile holding vUm (world)
private:
   Excitation ex_;
   std::vector<double> amp_, zMid_;
   bool sheet_ = false;
};

} // namespace sim
