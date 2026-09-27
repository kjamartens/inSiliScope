///////////////////////////////////////////////////////////////////////////////
// FILE:          WidefieldRender.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The WideField imaging modality: every labelled dye emits at
//                once. The dyes are binned into world-anchored z-planes
//                (isc_density3d_in_window, per population), each plane is
//                convolved with the emission PSF at its defocus (FFT), and
//                the sum is cropped to the FOV and binned to camera pixels.
//                The Poisson draw then happens once per camera pixel in the
//                existing ApplyNoiseChain.
//
//                Photophysics in physical units, per dye at pattern value I:
//                  sigma = ln(10) 1000 eps / N_A      absorption cross-section
//                  k_em  = QY sigma Phi I             emitted photons / s
//                  eta   = (1 - sqrt(1 - (NA/n)^2)) / 2   collection efficiency
//                  S     = exp(-D / B)                surviving fraction at dose D
//                Camera photons per frame from nb bleaching / np persistent
//                dyes, frame-start dose D0, frame dose dD = k_em t_exp:
//                  nb eta B exp(-D0/B) (1 - exp(-dD/B))   (exact frame integral)
//                  np eta dD
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include "Fft2d.h"
#include "Illumination.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sim {

class CellFieldSource;
struct PsfKernelCache;

struct WidefieldPhotophysics
{
   double excitationPhotonsPerUm2PerSec = 1.6e9; // Phi at pattern value 1 (~0.05 W/cm^2 at 640 nm)
   double quantumYield = 0.7;
   double photonBudget = 5000.0;   // emitted photons per dye before it bleaches (1/e); <= 0: never bleaches
   double extinctionCoeff = 270000.0; // M^-1 cm^-1

   double CrossSectionUm2() const;                // 3.8235e-13 * eps
   double EmissionRatePerSec(double I) const;     // k_em
   double HalfTimeSec(double I) const;            // B ln2 / k_em (infinity if it never bleaches)
   bool Bleaches() const { return photonBudget > 0.0; }
};

double WidefieldCollectionEfficiency(double na, double immersionIndex);

// Camera photons per bleaching dye in one frame (before QE): the exact
// integral of eta k_em exp(-D/B) over the frame.
double WidefieldBleachingPhotons(double eta, double budget, double d0, double dD);

// Width (sigma, um) of the emission PSF -- the image of one dye -- at a
// defocus (um from the focal plane), for the Gaussian PSF model.
double WidefieldGaussianSigmaUm(double defocusUm, double lambdaNm, double na, double immersionIndex);

struct WidefieldGridSettings
{
   int upscale = 1;        // grid cells per camera pixel, per axis
   double zPlaneNm = 25.0; // dye plane thickness (world-anchored)
};

// Upscaled xy grid (world um) and z slab of a dye grid.
struct WidefieldGridSpec
{
   double x0Um = 0.0, y0Um = 0.0, pitchUm = 0.1;
   unsigned nx = 0, ny = 0;
   double zMinUm = -5.0, zMaxUm = 50.0, zPlaneUm = 0.025;
   bool operator==(const WidefieldGridSpec& o) const
   {
      return x0Um == o.x0Um && y0Um == o.y0Um && pitchUm == o.pitchUm && nx == o.nx && ny == o.ny &&
             zMinUm == o.zMinUm && zMaxUm == o.zMaxUm && zPlaneUm == o.zPlaneUm;
   }
   bool operator!=(const WidefieldGridSpec& o) const { return !(*this == o); }
};

// Dye counts per population on the occupied world planes of a grid: plane i
// spans [(k0 + i) zPlane, (k0 + i + 1) zPlane).
struct WidefieldDyeGrid
{
   WidefieldGridSpec spec;
   long k0 = 0;
   unsigned nz = 0;
   std::vector<float> bleaching, persistent; // nz * ny * nx each; empty if that population has no dye
   long nBleaching = 0, nPersistent = 0;
   double PlaneCentreUm(unsigned i) const { return (k0 + static_cast<long>(i) + 0.5) * spec.zPlaneUm; }
};

// Two passes: a z histogram (1 x 1 x planes) finds the occupied planes, then
// each population is binned over just those. False (with err) on a failure.
bool BuildWidefieldDyeGrid(CellFieldSource& src, const WidefieldGridSpec& spec, WidefieldDyeGrid& out,
                           std::string& err);

// Emission PSF on the grid's pitch, as a set of planes indexed by an integer
// p at defocus PlaneDefocusUm(p). Dye planes between two PSF planes are
// deposited on both with linear weights (= convolving with a linearly
// z-blended PSF).
class WidefieldPsf
{
public:
   virtual ~WidefieldPsf() = default;
   virtual double PlaneCoord(double defocusUm) const = 0; // continuous plane index
   virtual int MinPlane() const = 0;
   virtual int MaxPlane() const = 0;
   // Natural kernel radius (grid cells) covering planes [pLo, pHi].
   virtual int Radius(int pLo, int pHi) const = 0;
   // (2R+1)^2 kernel of plane p, row-major, centred on cell (R, R).
   virtual void Kernel(int p, int R, std::vector<float>& out) const = 0;
};

// Gaussian of WidefieldGaussianSigmaUm, planes every stepUm of defocus
// (unbounded), normalised to sum 1.
class GaussianWidefieldPsf : public WidefieldPsf
{
public:
   GaussianWidefieldPsf(double pitchUm, double lambdaNm, double na, double immersionIndex, double stepUm = 0.1);
   double PlaneCoord(double defocusUm) const override { return defocusUm / step_; }
   int MinPlane() const override { return -1000000; }
   int MaxPlane() const override { return 1000000; }
   int Radius(int pLo, int pHi) const override;
   void Kernel(int p, int R, std::vector<float>& out) const override;

private:
   double Sigma(int p) const;
   double pitch_, lambdaNm_, na_, n_, step_;
};

// The vectorial kernel planes (PsfKernelCache), resampled to the grid pitch:
// each grid cell is the sum of its (os/upscale)^2 oversampled cells, placed
// exactly as SplatPsfKernel centres them (Nearest). Planes are the cache's
// own z planes; not renormalised (as the SR splat).
class VectorialWidefieldPsf : public WidefieldPsf
{
public:
   // upscale must divide cache.oversampling (see ValidUpscale).
   VectorialWidefieldPsf(const PsfKernelCache& cache, int upscale);
   // The largest divisor of oversampling that is <= requested (>= 1).
   static int ValidUpscale(int oversampling, int requested);
   double PlaneCoord(double defocusUm) const override;
   int MinPlane() const override { return 0; }
   int MaxPlane() const override;
   int Radius(int pLo, int pHi) const override;
   void Kernel(int p, int R, std::vector<float>& out) const override;

private:
   const PsfKernelCache& c_;
   int r_; // oversampled cells per grid cell
   int s0_; // first oversampled index of grid cell 0
};

// World-anchored emitted-photon dose (photons per dye) in sparse 256 x 256
// tiles of `pitch`; cell i = floor(x / pitch). A grid of the same pitch maps
// its columns one-to-one onto cells whatever its offset (column j -> the cell
// holding its centre), so reads and deposits stay consistent.
class BleachField
{
public:
   void Reset(double pitchUm);
   double Pitch() const { return pitch_; }
   bool Empty() const { return tiles_.empty(); }
   // Dose of the cell holding each grid cell's centre; grid corner (x0, y0).
   void DoseOver(double x0Um, double y0Um, unsigned nx, unsigned ny, std::vector<float>& out) const;
   // Adds dDPerUnitI x the pattern value at each cell centre, over the
   // pattern's whole support around the axis (world um).
   void Deposit(const IlluminationPattern& pattern, double axisXUm, double axisYUm, double dDPerUnitI);

private:
   static constexpr int kTile = 256;
   std::map<std::pair<long long, long long>, std::vector<float>> tiles_;
   double pitch_ = 0.0;
};

struct WidefieldSceneSpec
{
   // FOV: world um of pixel (0, 0)'s centre (CellFieldQuery::originX/YUm).
   double originXUm = 0.0, originYUm = 0.0;
   unsigned width = 0, height = 0;
   double pixelUm = 0.1;
   double focusWorldUm = 0.0; // world z of the focal plane
   // Dyes within +/- slabHalfUm of slabCentreUm; slabHalfUm <= 0 = all
   // (histogrammed over [-5, 50] um).
   double slabCentreUm = 0.0, slabHalfUm = 0.0;
   WidefieldGridSettings grid;
   double marginUm = 2.0;     // grid margin beyond the FOV, within the illumination support
   double kernelCapUm = 7.0;  // kernel radius cap
   long worldVersion = 0;     // bump when the CellFieldSource's world changes
   long psfVersion = 0;       // bump when the WidefieldPsf changes
   WidefieldPhotophysics phot;
   double eta = 0.3;          // collection efficiency
   double exposureSec = 0.05;
};

class WidefieldScene
{
public:
   // (Re)builds whatever changed: the dye grid (pose, slab, grid settings,
   // world), the PSF planes (focus, PSF), the illumination and dose maps and
   // the persistent spectrum. False (with err) on a failure.
   bool Update(CellFieldSource& src, const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
               const WidefieldPsf& psf, std::string& err);
   // The same from a dye grid built elsewhere (tests, synthetic samples);
   // its spec must be GridSpecFor(pattern, spec).
   bool UpdateFromGrid(const WidefieldDyeGrid& grid, const IlluminationPattern& pattern,
                       const WidefieldSceneSpec& spec, const WidefieldPsf& psf, std::string& err);
   // The grid a spec renders on: the FOV at pitch pixel / upscale, grown by
   // the margin where the pattern still excites, and the z slab.
   static WidefieldGridSpec GridSpecFor(const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                                        unsigned* marginX = nullptr, unsigned* marginY = nullptr);

   const WidefieldGridSpec& Grid() const { return grid_.spec; }
   double AxisXUm() const { return axisX_; }
   double AxisYUm() const { return axisY_; }
   // Emitted photons per unbleached dye in one frame, per grid column.
   const std::vector<float>& FrameDose() const { return dD_; }
   long Dyes() const { return grid_.nBleaching + grid_.nPersistent; }
   long BleachingDyes() const { return grid_.nBleaching; }
   long ClampedDyes() const { return clamped_; }
   unsigned FftSize() const { return fft_.N(); }
   int KernelRadius() const { return R_; }
   int PsfPlanes() const { return nP_; }

   // Per-column camera photons per bleaching dye for a frame: fresh sample
   // (D0 = framesBefore x dD) or from a dose map (BleachField::DoseOver).
   void FreshBleachWeights(double framesBefore, std::vector<float>& wb) const;
   void BleachWeightsFromDose(const std::vector<float>& d0, std::vector<float>& wb) const;

   // Makes wb the cached bleaching spectrum's weights (one FFT per PSF plane).
   void SetBleachWeights(const std::vector<float>& wb);
   // True (and c) if wb = c x the cached bleaching weights.
   bool ScalarOfBleaching(const std::vector<float>& wb, double& c) const;
   // Adds c x the bleaching image + the persistent image to cam (width x
   // height). Thread-safe (const): scratch is the caller's.
   void RenderScaled(double c, std::vector<float>& cam, std::vector<cfloat>& scratch) const;
   // One frame: the fast path when wb is a multiple of the cached weights,
   // else SetBleachWeights first. Adds to cam.
   void RenderFrame(const std::vector<float>& wb, std::vector<float>& cam);
   bool LastFrameFast() const { return lastFast_; }

private:
   void Finish(bool gridChanged, const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
               const WidefieldPsf& psf, unsigned mx, unsigned my);
   void BuildPlanes(const WidefieldPsf& psf, double kernelCapUm);
   // Sum over PSF planes of F(src_p x w) F(K_p), into S.
   void Spectrum(const std::vector<float>& src, const std::vector<float>& w, std::vector<cfloat>& S) const;

   WidefieldSceneSpec spec_;
   bool haveSpec_ = false;
   WidefieldDyeGrid grid_;
   unsigned fovX0_ = 0, fovY0_ = 0; // grid cell of the FOV's corner
   double axisX_ = 0.0, axisY_ = 0.0;
   std::vector<float> illum_, dD_, wp_;
   // PSF planes pMin_ .. pMin_ + nP_ - 1: per-plane source maps and kernels.
   int pMin_ = 0, nP_ = 0, R_ = 0;
   long clamped_ = 0;
   std::vector<float> srcB_, srcP_, kernels_;
   Fft2d fft_;
   std::vector<cfloat> Sp_, Sb_;
   std::vector<float> wbRef_;
   std::vector<cfloat> scratch_;
   bool bleachValid_ = false, lastFast_ = false;
};

} // namespace sim
