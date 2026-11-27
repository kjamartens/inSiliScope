///////////////////////////////////////////////////////////////////////////////
// FILE:          ScanSpot.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Confocal laser scanning (CSLM) excitation (2026-11-27,
//                spec/ALGORITHM.md "Confocal laser scanning (CSLM)
//                excitation"): a focused spot scanned over a square field,
//                the camera detecting (no pinhole yet), so a frame holds the
//                light the spot left over its exposure.
//
//                The spot: a uniformly filled pupil of NA_ex = fill x NA
//                (angles past the critical angle dropped: NA_ex <= 0.999 n_s)
//                focused into the medium, scalar Debye (sine condition,
//                sqrt(cos) apodization, no aberrations):
//                   I(rho, z) ~ |int_0^thx sqrt(cos t) sin t J0(k rho sin t)
//                                exp(i k z cos t) dt|^2,
//                numerically up to k |z| (1 - cos thx) = kDebyeAxialRad, then
//                its geometric limit cos^3 t / z^2 (tan t = rho / |z|). Each
//                plane holds unit power (to 99 % of the rays and 8 lambda /
//                NA_ex: the rest renormalized). Kept per plane as two tables:
//                C(a, b) = int_{-inf}^{a} I(sqrt(x^2 + b^2)) dx (a line's sweep)
//                and D(a, b) = int_{-inf}^{b} C(a, b') db' (a band of lines).
//
//                The scan: N = ceil(field / pixel) lines along x (fast axis,
//                unidirectional), in +y; dwell tau per pixel, so the spot moves
//                at v = pixel / tau, then a flyback (dark); line time N tau +
//                flyback, scan time T = N x line time; free running from t = 0
//                of the frame counter (frame F: [F E, (F + 1) E]). A frame's
//                light at a point is (P / E) x (1 / v) x the sum over the lines
//                it swept of C(x - xs) - C(x - xe): exact for the continuous
//                sweep; the lines far from the point (|b| > the core radius,
//                where the spot is smooth on the line spacing) as their strips'
//                integral (D), the near ones line by line.
//
//                Power: the laser setting is the irradiance averaged over the
//                scan field and time (flyback included): P = setting x field^2 /
//                duty. Values here are relative to the setting (a whole scan's
//                interior = 1).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Fft2d.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sim {

class BfpLight;

struct ScanLightParams
{
   std::vector<double> lines;       // the lit laser lines (nm); empty: lambdaRefNm
   double lambdaRefNm = 640;
   double na = 1.4, nS = 1.337;
   double fieldUm = 12.8;           // the scan field's side (the camera's field when the option is 0)
   double pixelUm = 0.1;            // the scan pixel (line spacing and pixel along the line)
   double dwellSec = 2e-6;          // per scan pixel
   double flybackSec = 1e-5;        // per line, dark
   double fill = 1.0;               // the pupil's fill (NA_ex = fill x NA)
   double zSampleUm = 12;           // the spot's z range (|z| from the focal plane); beyond: the end plane
};

// The raster.
struct ScanProgram
{
   int N = 1;                // lines (and pixels per line)
   double p = 0.1;           // um
   double tau = 2e-6;        // s per pixel
   double fb = 1e-5;         // s per line, dark
   double x0 = 0, y0 = 0;    // the field's corner (um from the axis): -N p / 2
   double lineSec = 0, scanSec = 0, sweepSec = 0;   // N tau + fb, N lineSec, N tau
   double v = 1;             // um / s along a line
   double duty = 1;          // sweepSec / lineSec
   double K = 1;             // field^2 / (duty v): the light per (second x 1/um) of sweep, relative to the setting
   double LineY(int k) const { return y0 + (k + 0.5) * p; }
};

// Lines k1..k2 swept from xs to xe, w times.
struct ScanRun
{
   int k1 = 0, k2 = 0;
   double xs = 0, xe = 0, w = 1;
};

// The frame's runs over [t0, t0 + E] (s), and the frame's whole scans in it.
void ScanRunsOf(const ScanProgram& g, double t0, double E, std::vector<ScanRun>& runs);
// The frames after which the scan repeats (1: every frame is whole scans, the mean), up to kScanMaxPeriod; 0: none.
int ScanPeriodFrames(double expSec, double scanSec);
constexpr int kScanMaxPeriod = 64;

// A plane's light on a square grid (n x n from (x0, x0), step dx; relative to the setting).
struct ScanPlaneImage
{
   std::vector<float> v;
   double x0 = 0, dx = 1;
   int n = 0;
};
constexpr size_t kScanImageBytes = size_t(96) << 20;   // the plane images kept (the frames' mean-field plane factors)

// One line's focused spot (unit power per plane), as tables per plane of |z| from the focus.
class FocusedSpot
{
public:
   FocusedSpot() = default;
   FocusedSpot(const FocusedSpot& o)
      : nm_(o.nm_), naEx_(o.naEx_), nS_(o.nS_), k_(o.k_), sinX_(o.sinX_), cosX_(o.cosX_), planes_(o.planes_),
        peak_(o.peak_), fwhmLat_(o.fwhmLat_), fwhmAx_(o.fwhmAx_)
   {
   }
   void Make(double nm, double naEx, double nS, double zMaxUm);
   double Nm() const { return nm_; }
   int PlaneCount() const { return static_cast<int>(planes_.size()); }
   double PlaneZ(int j) const { return planes_[static_cast<size_t>(j)].z; }
   double PlaneReachUm(int j) const { return planes_[static_cast<size_t>(j)].R; }
   double PlaneStepUm(int j) const { return planes_[static_cast<size_t>(j)].h; }
   // The light of the runs at (x, y) on plane j (sum of w x the swept C differences: 1/um), and at |z| (planes
   // interpolated linearly).
   double PlaneSum(int j, const ScanProgram& g, const std::vector<ScanRun>& runs, double x, double y) const;
   double Sum(const ScanProgram& g, const std::vector<ScanRun>& runs, double x, double y, double zRel) const;
   // One row of plane j on x0 + i dx (i < nx), added to out x scale (the runs far from x: their row value once).
   void PlaneRow(int j, const ScanProgram& g, const std::vector<ScanRun>& runs, double y, double x0, double dx,
                 int nx, double scale, float* out) const;
   // The bracketing planes of |z| and the weight of the upper one (j1 = j0 at the ends).
   void Bracket(double zRel, int& j0, int& j1, double& t) const;
   // Plane j's light of the runs (x norm) on its grid (FFT; see ScanSpot.cpp), and that grid.
   void PlaneImage(int j, const ScanProgram& g, const std::vector<ScanRun>& runs, double norm, std::vector<float>& out,
                   double& x0, double& dx, int& n) const;
   void Geometry(int j, const ScanProgram& g, double& x0, double& dx, int& n, int& m) const;
   // In-focus readouts: the peak (1/um^2, unit power), the lateral and axial FWHM (um).
   double PeakPerUm2() const { return peak_; }
   double FwhmLateralUm() const { return fwhmLat_; }
   double FwhmAxialUm() const { return fwhmAx_; }
   size_t Bytes() const;

private:
   struct Plane
   {
      double z = 0, R = 1, h = 0.1, Rc = 0, hr = 0.01;
      int M = 1;
      std::vector<float> I;   // the radial profile (unit power), rho = i hr
      std::vector<float> C;   // (M + 1) rows (b = j h) x (2M + 1) columns (a = -R + i h)
      std::vector<float> D;   // (2M + 1) rows (b = -R + j h) x (2M + 1) columns
      std::vector<float> P;   // C(R, b), b = j h (M + 1)
      std::vector<float> Q;   // D(R, b), b = -R + j h (2M + 1)
   };
   double CAt(const Plane& q, double a, double b) const;
   double DAt(const Plane& q, double a, double b) const;
   double PAt(const Plane& q, double b) const;
   double QAt(const Plane& q, double b) const;
   double RunSum(const Plane& q, const ScanProgram& g, const ScanRun& r, double x, double y, bool interior) const;
   void MakePlane(Plane& q, double z) const;
   double nm_ = 640, naEx_ = 1, nS_ = 1.337, k_ = 1, sinX_ = 0.5, cosX_ = 0.8;
   std::vector<Plane> planes_;
   double peak_ = 0, fwhmLat_ = 0, fwhmAx_ = 0;
   // Per plane, the cell-mean kernel's spectrum on its grid (made on first use).
   struct Kernel
   {
      RealFft2d fft;
      std::vector<cfloat> spec;
   };
   const Kernel& KernelOf(int j, const ScanProgram& g) const;
   mutable std::mutex kernelMutex_;
   mutable std::vector<std::unique_ptr<Kernel>> kernels_;
};

// The scan and a spot per line (kept by the BfpLight it makes).
class ScanLight
{
public:
   const ScanProgram& Program() const { return prog_; }
   const ScanLightParams& Params() const { return params_; }
   int LineCount() const { return static_cast<int>(spots_.size()); }
   const FocusedSpot& Spot(int l) const { return spots_[static_cast<size_t>(l)]; }
   // The light (relative to the setting) of line l at (x, y, zRel) under runs and a norm (K / E: ScanNorm).
   double At(int l, const std::vector<ScanRun>& runs, double norm, double x, double y, double zRel) const
   {
      return norm * spots_[static_cast<size_t>(l)].Sum(prog_, runs, x, y, zRel);
   }
   // A frame's runs and norm (frame F of exposure E).
   void FrameRuns(long long frame, double expSec, std::vector<ScanRun>& runs, double& norm) const;
   // The long-run mean's (one whole scan / T).
   void MeanRuns(std::vector<ScanRun>& runs, double& norm) const;
   // The spot's peak irradiance per the setting (the field mean): field^2 / duty x the in-focus peak.
   double PeakPerSetting(int l) const;
   // Line l's plane j under runs x norm on its grid (FocusedSpot::PlaneImage), memoised (kScanImageBytes).
   std::shared_ptr<const ScanPlaneImage> PlaneImageOf(int l, int j, const std::vector<ScanRun>& runs, double norm) const;
   size_t Bytes() const;

private:
   friend struct ScanLightBuilder;
   ScanLightParams params_;
   ScanProgram prog_;
   std::vector<FocusedSpot> spots_;
   mutable std::mutex imageMutex_;
   mutable std::list<std::pair<uint64_t, std::shared_ptr<const ScanPlaneImage>>> images_;
};

// The CSLM light as a BfpLight (memoised by the parameters): its cycle mean a volume per line (relative to the focal
// plane) for the kinetics, the history and the imager background, and its ScanLight (BfpLight::Scan) for the frames.
std::shared_ptr<const BfpLight> MakeScanLight(const ScanLightParams& p, std::string& err);

} // namespace sim
