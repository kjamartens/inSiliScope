///////////////////////////////////////////////////////////////////////////////
// FILE:          SimPattern.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Structured illumination (2026-10-30, spec/ALGORITHM.md
//                "Structured illumination"): the light as a set of coherent
//                beams, each a spot in the objective's back focal plane (BFP)
//                -- position in u units (Excitation.h: sin theta_s = |u| in the
//                medium; |u| > 1 evanescent), spot size (the envelope's width in
//                the sample, its Fourier pair), entry tilt (the envelope's
//                centre), amplitude, phase, polarization (s / p angle and an
//                ellipticity phase), a phase step per phase index and the laser
//                line it carries -- and the frame program that steps the
//                pattern's phases and rotates it with the frame number. The
//                pattern of one laser line at a frame is a list of terms
//                (CompilePattern): per beam its intensity (the DC term: the
//                single-beam physics of Excitation.h, polarized) and per pair
//                the fringe 2 Re(E_j E_l*) -- a cosine in the lateral
//                wavevector k0 n_s (u_j - u_l) and the axial kz_j - kz_l, under
//                the evanescent decays. The envelopes (one per beam, sheared and
//                stretched by the beam's angle) are Excitation's: a term names
//                its beams. Pure: no engine state, no spec.
//
//                The frame program (FrameProgram): frame f -> (angle, phase):
//                angle a rotates every beam by a x 180 / nAngles degrees (+ the
//                first angle), phase p adds step_j x (p + phase0) / nPhases
//                turns to beam j. A pair with steps +s / -s moves its fringe by
//                2 s / nPhases of a period per phase; the lattice's x and y
//                pairs step 2 : 3 (the user's scheme, 13 phases). The orders'
//                phase shifts are distinct mod nPhases for every built-in type
//                (OrdersSeparate: the reconstruction's premise; ctest
//                sim_pattern). Phase drift: each beam's phase a random walk per
//                frame (counter-based pcg4d on seed ^ "SIMP", frame, beam: one
//                path per seed on every consumer) plus a linear drift along the
//                beam's step.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace sim {

enum SimType { SIM_OFF = 0, SIM_2D = 1, SIM_3D = 2, SIM_LATTICE_2D = 3, SIM_LATTICE_3D = 4, SIM_CUSTOM = 5 };
enum SimPolarization { SIM_POL_S = 0, SIM_POL_P = 1, SIM_POL_CIRCULAR = 2, SIM_POL_LINEAR_X = 3, SIM_POL_UNPOLARIZED = 4 };
enum SimOrder { SIM_ORDER_ANGLE_MAJOR = 0, SIM_ORDER_PHASE_MAJOR = 1 };
const std::vector<std::string>& SimTypeNames();           // Off, SIM2D, SIM3D, LatticeSIM2D, LatticeSIM3D, Custom
const std::vector<std::string>& SimPolarizationNames();   // S, P, Circular, LinearX, Unpolarized
const std::vector<std::string>& SimOrderNames();          // AngleMajor, PhaseMajor
const std::vector<std::string>& SimAnchorNames();         // Focus, Coverslip (sim-anchor: where the beams are in phase)
// The side beams' radius a type pick sets (u; estimates: 2D SIM near the pupil edge, 3D and lattice propagating).
double SimTypeDefaultRadius(int type);

// One beam: a spot in the BFP. The matrix columns, in this order (BeamColumnNames): ux uy spot tiltX tiltY amp phase
// pol ellip phaseStep nm.
struct Beam
{
   double ux = 0, uy = 0;          // position in the BFP, u units (the critical radius = 1)
   double spotUm = 0;              // the Gaussian spot's 1/e^2 radius in the pupil, um; 0 = the legacy width (illum-fwhm)
   double tiltXDeg = 0, tiltYDeg = 0;   // the bundle's entry angle into the BFP: the envelope's centre moves by f tan(tilt)
   double amp = 1;                 // field amplitude over the reference beam's
   double phaseTurns = 0;          // its phase, turns
   double polDeg = 0;              // the polarization angle: 0 s (perpendicular to the plane of incidence), 90 p, signed
                                   // (-180, 180]; below kUnpolarizedBelowDeg (kUnpolarizedDeg): unpolarized
   double ellipTurns = 0;          // the phase of the p part over the s part, turns (0.25: circular)
   double phaseStep = 0;           // turns per phase index, in units of 1 / nPhases
   double nm = 0;                  // the laser line it carries (0 = every line; reserved: the powers stay the lines')
   double Radius() const;          // |u|
   double AzimuthRad() const;      // atan2(uy, ux)
};
constexpr int kBeamColumns = 11;
constexpr double kUnpolarizedDeg = -999, kUnpolarizedBelowDeg = -180;
const std::vector<std::string>& BeamColumnNames();
double BeamColumn(const Beam& b, int column);
void SetBeamColumn(Beam& b, int column, double v);

// The beams and their program.
struct BeamSet
{
   int type = SIM_OFF;
   int polarization = SIM_POL_S;
   std::vector<Beam> beams;
   int phases = 1, angles = 1;
   int order = SIM_ORDER_ANGLE_MAJOR;
   double angle0Deg = 0;          // the first angle
   double phase0 = 0;             // added to the phase index (fractional allowed)
   double driftTurnsPerSqrtSec = 0, driftTurnsPerSec = 0;   // phase drift: the random walk's sigma, the linear part
   int Period() const { return phases * angles; }
   bool Varies() const { return Period() > 1 || driftTurnsPerSqrtSec > 0 || driftTurnsPerSec != 0; }
};
// The beams of a type: Off one beam at (radius, 0); SIM2D two at +-radius (phases 3, angles 3, steps +1 / -1); SIM3D
// those and the centre (centreAmp; phases 5); LatticeSIM2D +-x and +-y (steps +1 / -1, +2 / -2; phases 9, angles 1);
// LatticeSIM3D those and the centre (steps +2 / -2, +3 / -3; phases 13); Custom none (the matrix alone). Every beam
// polarized per `polarization` (S: 0; P: 90; Circular: 45 at a quarter turn; LinearX: the pupil field along the lab
// x axis, atan2(cos phi, -sin phi), signed; Unpolarized: polDeg kUnpolarizedDeg).
BeamSet MakeBeams(int type, double radiusU, int polarization, double centreAmp);
// A matrix row (a column's value given) laid over beam j: j < count overrides the columns given, j >= count adds a beam
// (the missing columns at Beam's defaults).
void ApplyBeamRow(BeamSet& set, int j, const std::vector<std::pair<int, double>>& columns);
// The matrix as text: rows "ux uy spot tiltX tiltY amp phase pol ellip phaseStep nm" separated by ';' (or newlines),
// values by spaces (or commas); a row may give fewer columns (the rest unchanged / default). Parse: false (err) on a
// bad number or more than kBeamColumns values in a row; rows out: one vector of values per row, ragged.
bool ParseBeamMatrix(const std::string& text, std::vector<std::vector<double>>& rows, std::string& err);
std::string FormatBeamMatrix(const std::vector<Beam>& beams);

// The frame program: frame f -> its angle and phase indices (the program's order), and a scale reserved for a
// stroboscopic light (1).
struct FrameLight
{
   int angle = 0, phase = 0;
   double scale = 1;
};
FrameLight FrameLightAt(const BeamSet& set, long long frame);
// The beams at a frame: every beam rotated by rotDeg about the BFP centre, its phase in turns (the program's step, the
// drift). seed / expSec: the phase drift (none without drift settings).
struct PatternState
{
   long long frame = 0;
   FrameLight light;
   double rotDeg = 0;
   std::vector<double> phaseTurns;   // per beam
   std::vector<Beam> beams;          // rotated, phased (phaseTurns applied)
   bool operator==(const PatternState& o) const { return rotDeg == o.rotDeg && phaseTurns == o.phaseTurns; }
};
// The drift of beam j's phase at frame f (turns): the walk sum_{i < f} sigma sqrt(expSec) g_i (pcg4d on seed ^ "SIMP",
// counter i, pixel j; O(f)) + step_j x rate x f x expSec.
double PhaseDriftTurns(const BeamSet& set, uint32_t seed, int beam, long long frame, double expSec);
PatternState PatternStateAt(const BeamSet& set, long long frame, uint32_t seed, double expSec);

// The field one beam makes in the sample at one line, over the epi amplitude t0 (Excitation.h: F = |E|^2 is the
// single beam's factor), in the beam's own frame (x along its azimuth in the plane of incidence, y = s, z up): the
// Fresnel amplitudes t_s, t_p below the critical angle (bornwolf1999 1.5.2), Axelrod's evanescent components with
// their phases above it (axelrod1984 eqs. 3-8; their |.|^2 are the I_s, I_p of Excitation::Make). The polarization
// state cos a E_s + sin a e^{i 2 pi ellip} E_p; unpolarized (polDeg below kUnpolarizedBelowDeg): E_s (the compile averages
// the s and p runs).
struct BeamField
{
   bool clipped = false, evanescent = false;
   double u = 0, cosI = 1, sinS = 0, cosS = 1, tanS = 0;
   double decayUm = 0;              // the intensity's 1/e depth (evanescent), else 0
   double kzOverK = 1;              // the propagating z wavevector over k0 n_s (cosS), 0 when evanescent
   std::complex<double> ex, ey, ez; // the field over t0 (|E|^2 = the beam's F)
   double Intensity() const { return std::norm(ex) + std::norm(ey) + std::norm(ez); }
};
BeamField BeamFieldOf(const Beam& b, double lambdaNm, double nS, double nI, double na, int polarization,
                      bool forceS = false, bool forceP = false);

// The pattern of one line at a state: I(r) = sum of terms amp x envelope_jl(r) x exp(-z / decay) x cos(qx x + qy y +
// kz z + phase); a DC term (j == l) has q = kz = 0 and phase 0. Lateral q = k0 n_s (u_j - u_l) in rad/um (x, y um from
// the optical axis), kz = k0 n_s (kz_j - kz_l) for propagating beams, 1 / decay = 1 / (2 d_j) + 1 / (2 d_l) over the
// evanescent ones. Beams with another line (nm > 0 and != lambda) are left out.
struct PatternTerm
{
   int j = 0, l = 0;
   double qx = 0, qy = 0, kz = 0;
   double decayUm = 0;   // 0: none
   double amp = 0, phase = 0;
};
struct Pattern
{
   std::vector<PatternTerm> terms;
   std::vector<BeamField> fields;   // per beam (the s run's for unpolarized beams)
   // The lateral fringe periods (um) of the fringe terms, distinct, largest first; empty for one beam.
   std::vector<double> PeriodsUm() const;
   // The intensity at r over the envelopes env[j] (sqrt(env_j env_l) for a fringe term).
   double At(double x, double y, double z, const double* env) const;
   // At averaged over a ball of radius r (um) about (x, y, z), the envelopes those at the centre (a fiducial bead's
   // volume, 2026-10-31: a bead wider than a fringe is never dark): per term the ball's form factor
   // 3 (sin k - k cos k) / k^3 at k = r |(qx, qy, kz + i / decay)| (complex: the decay inside the ball), exact for
   // the plane-wave terms (the coverslip's cut of the decay at z = 0 is not: the ball is taken whole).
   double BallMean(double x, double y, double z, double r, const double* env) const;
   // The same over the DC terms alone (j == l): the frame's beams' incoherent sum over the ball.
   double DcBallMean(double z, double r, const double* env) const;
};
// zRefUm: the height where the beams are in phase (their phases as given): the terms' phases carry -kz zRefUm, so the
// pattern's axial structure is fixed to that plane (the focal plane under sim-anchor Focus, the coverslip under
// Coverslip; 2026-10-31). The evanescent decays are from the coverslip whatever it is.
Pattern CompilePattern(const PatternState& st, double lambdaNm, double nS, double nI, double na, int polarization,
                       double zRefUm = 0.0);

// Every order's phase shift per phase index is distinct mod nPhases (a fringe between beams j, l shifts by
// (step_j - step_l) / nPhases of a turn; orders are the distinct lateral q up to sign): the separation the
// reconstruction needs. why: the first collision, or empty.
bool OrdersSeparate(const BeamSet& set, std::string& why);

} // namespace sim
