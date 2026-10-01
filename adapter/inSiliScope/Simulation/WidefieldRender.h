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
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Fft2d.h"
#include "Illumination.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sim {

class CellFieldSource;
struct PsfKernelCache;

struct WidefieldPhotophysics
{
   double excitationPhotonsPerUm2PerSec = 4e8; // Phi at pattern value 1 (~0.0125 W/cm^2 at 640 nm)
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

// World-anchored upscaled xy grid and z slab of a dye grid: cell (i, j) is
// world [(ix0 + i) pitch, (ix0 + i + 1) pitch) x [(iy0 + j) pitch, ...), the
// same cells whatever the stage pose, so a dye is binned the same way from
// every pose and BleachField cells map one-to-one onto grid cells.
struct WidefieldGridSpec
{
   long ix0 = 0, iy0 = 0;
   double x0Um = 0.0, y0Um = 0.0, pitchUm = 0.1; // x0Um = ix0 pitch
   unsigned nx = 0, ny = 0;
   double zMinUm = -5.0, zMaxUm = 50.0, zPlaneUm = 0.025;
   bool operator==(const WidefieldGridSpec& o) const
   {
      return ix0 == o.ix0 && iy0 == o.iy0 && pitchUm == o.pitchUm && nx == o.nx && ny == o.ny &&
             zMinUm == o.zMinUm && zMaxUm == o.zMaxUm && zPlaneUm == o.zPlaneUm;
   }
   bool operator!=(const WidefieldGridSpec& o) const { return !(*this == o); }
   bool SameRect(const WidefieldGridSpec& o) const
   {
      return ix0 == o.ix0 && iy0 == o.iy0 && pitchUm == o.pitchUm && nx == o.nx && ny == o.ny &&
             zPlaneUm == o.zPlaneUm;
   }
};

// Dye counts per population on the occupied world planes of a grid: plane i
// spans [(k0 + i) zPlane, (k0 + i + 1) zPlane). Dense; the reference layout
// for tests and synthetic samples.
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

// Sparse dye counts: per world plane k and population, (cell, count) pairs,
// cell = row-major index in the grid rect it was made for.
struct WidefieldSparsePlane
{
   std::vector<uint32_t> cell[2]; // [0] bleaching, [1] persistent
   std::vector<float> count[2];
   bool Empty(int pop) const { return cell[pop].empty(); }
};

struct WidefieldDyePlanes
{
   WidefieldGridSpec rect; // the xy rect (zMin/zMax: the column fetched)
   std::map<long, WidefieldSparsePlane> planes; // world plane k -> dyes
   long nBleaching = 0, nPersistent = 0;
   static WidefieldDyePlanes FromGrid(const WidefieldDyeGrid& g);
};

// World-anchored dye tiles (kTile x kTile cells, every occupied plane of the
// z column), filled from isc_density3d_in_window on first use and assembled
// into any grid rect of the same pitch and plane thickness. A small stage move
// then queries only the tiles that newly enter the view. Thread-safe (the
// live loop and a prefetch worker share one); each caller passes its own
// CellFieldSource. Cleared when the key (pitch, plane, column, world) changes;
// least recently used tiles are dropped beyond the memory budget.
class WidefieldDyeTiles
{
public:
   static constexpr int kTile = 64;
   explicit WidefieldDyeTiles(size_t budgetBytes = 0); // 0 = default for the platform
   // The planes of rect (its zMin/zMax: the z column, world um) for world
   // version `world`. False (with err) on a failure.
   bool Planes(CellFieldSource& src, const WidefieldGridSpec& rect, long world, WidefieldDyePlanes& out,
               std::string& err);
   size_t Bytes() const;
   void Clear();

private:
   struct Tile
   {
      std::map<long, WidefieldSparsePlane> planes; // cells relative to the tile
      long nBleaching = 0, nPersistent = 0;
      size_t bytes = 0;
      unsigned long long used = 0;
   };
   bool Fill(CellFieldSource& src, long tx, long ty, Tile& t, std::string& err) const;
   mutable std::mutex mutex_;
   std::map<std::pair<long, long>, std::shared_ptr<Tile>> tiles_;
   double pitch_ = 0.0, zPlane_ = 0.0, zMin_ = 0.0, zMax_ = 0.0;
   long world_ = -1;
   size_t bytes_ = 0, budget_ = 0;
   unsigned long long clock_ = 0;
};

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

// The diffraction kernel planes (PsfKernelCache), resampled to the grid pitch:
// each grid cell is the sum of its (os/upscale)^2 oversampled cells, placed
// exactly as SplatPsfKernel centres them (Nearest). Planes are the cache's
// own z planes; not renormalised (as the SR splat).
class KernelWidefieldPsf : public WidefieldPsf
{
public:
   // upscale must divide cache.oversampling (see ValidUpscale).
   KernelWidefieldPsf(const PsfKernelCache& cache, int upscale);
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
   // (the whole [-5, 50] um column).
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

// The camera image (before background and noise) of one focus, as linear
// channels: the persistent dyes, and the bleaching dyes split over a small
// basis of weight maps (see WidefieldScene). A frame is
// bin(max(0, P + sum_j a_j B_j)); cells are the FOV's grid cells (width x
// upscale by height x upscale), already shifted to the camera's sub-cell
// position.
struct WidefieldImages
{
   unsigned cw = 0, ch = 0, upscale = 1; // cell grid of the FOV
   std::vector<float> persistent;        // empty: none
   std::vector<std::vector<float>> bleach;
   // Adds the frame with bleach coefficients a (a.size() == bleach.size())
   // to cam (cw/upscale x ch/upscale camera pixels).
   void Render(const std::vector<double>& a, std::vector<float>& cam) const;
};

// The GPU's share of one focus (WidefieldGpu.wgsl): which dye planes of
// which channels meet which kernels, for a host (web/wf_gpu.js,
// WidefieldGpuD3D11) that keeps plane spectra resident across jobs by key.
// Keys are valid within one geometry value.
struct WidefieldGpuJob
{
   unsigned NX = 0, NY = 0;                       // FFT grid (powers of two, <= kMaxGpuFft)
   unsigned nx = 0, ny = 0;                       // dye grid; cell = y * nx + x
   unsigned fovX0 = 0, fovY0 = 0, cw = 0, ch = 0; // FOV cells in the grid
   double fracX = 0.0, fracY = 0.0;               // sub-cell shift
   unsigned long long geometry = 0;
   struct Kernel
   {
      int p;
      const std::vector<cfloat>* spec; // half spectrum, (NX / 2 + 1) x NY
   };
   std::vector<Kernel> kernels;
   struct Plane
   {
      unsigned long long key;
      std::vector<uint32_t> cells;
      std::vector<float> values; // count x weight
      float absSum = 0.0f;
   };
   std::vector<Plane> planes; // every plane the channels need, once (see WidefieldAccelerator::HasPlane)
   struct Dep
   {
      unsigned long long key;
      int p0, p1; // kernel planes; p1 only if two
      float w0, w1;
      bool two;
   };
   std::vector<std::vector<Dep>> channels; // [persistent if hasPersistent], then the bleach maps
   bool hasPersistent = false;
   static constexpr unsigned kMaxGpuFft = 2048;
};

// A synchronous GPU host the scene hands its focus work to (the adapter's
// D3D11 one). Images: one cw x ch image per job channel. False (err) on a
// failure: the scene then renders on the CPU from then on.
class WidefieldAccelerator
{
public:
   virtual ~WidefieldAccelerator() = default;
   // Whether the plane spectrum of key (in geometry) is resident: its dyes
   // are then left out of the job.
   virtual bool HasPlane(unsigned long long geometry, unsigned long long key) const = 0;
   virtual bool Images(const WidefieldGpuJob& job, std::vector<std::vector<float>>& out, std::string& err) = 0;
};

// The WideField renderer for one grid rect (a stage pose), any focus.
//
//  * Dye planes are world-anchored (zPlaneNm); each is convolved with the
//    PSF at its defocus by FFT. Its spectrum depends only on its dyes and
//    their weights, so it is cached (per plane, channel and resolution
//    level): a focus change only re-pairs cached plane spectra with cached
//    kernel spectra (one multiply-add per frequency and plane) plus one
//    inverse FFT per channel.
//  * Focus bands: a PSF plane whose spectrum is (almost) confined to the low
//    frequencies is convolved on a 2x or 4x coarser grid (dyes cloud-in-cell
//    binned, the binning deconvolved) and its spectrum embedded back. A plane
//    goes coarse only if the kernel energy it cannot represent plus the
//    binning's alias energy (white source) is below kBandEpsilon of its total
//    (<= 0.1% rms of that plane's light).
//  * Sub-cell stage positions: the image is resampled to the camera by a
//    Fourier phase ramp (exact for band-limited images: pitch <=
//    lambda / (4 NA)), so the dyes' binning does not depend on the pose.
//  * Bleaching: the frame's per-column weights wb are expressed in a basis
//    anchored at some frame: wb = sum_j a_j phi_j. With the stage and the
//    illumination fixed, wb evolves as wb_anchor exp(-t dD / B): columns
//    that share a frame dose dD share a factor (one basis map per distinct dD
//    value, <= kMaxGroups; a square illumination has one), otherwise a
//    Chebyshev expansion in dD (kChebTerms maps). Every frame is checked
//    against the basis (1e-5 of the peak weight) and re-anchored when it
//    does not fit, so the image is always that of wb.
class WidefieldScene
{
public:
   WidefieldScene();
   // Shares a dye tile cache (a prefetch scene and the live scene).
   void SetTiles(std::shared_ptr<WidefieldDyeTiles> tiles) { tiles_ = std::move(tiles); }
   const std::shared_ptr<WidefieldDyeTiles>& Tiles() const { return tiles_; }

   // (Re)builds whatever changed: the grid rect and dyes (pose, grid
   // settings, world), the kernels (PSF, rect), the illumination and dose
   // maps, the spectra and images of the focus. False (with err) on a
   // failure.
   bool Update(CellFieldSource& src, const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
               const WidefieldPsf& psf, std::string& err);
   // The same from a dye grid built elsewhere (tests, synthetic samples);
   // its rect must be GridSpecFor(pattern, spec)'s.
   bool UpdateFromGrid(const WidefieldDyeGrid& grid, const IlluminationPattern& pattern,
                       const WidefieldSceneSpec& spec, const WidefieldPsf& psf, std::string& err);
   // The grid a spec renders on: the world-anchored cells covering the FOV at
   // pitch pixel / upscale, grown by the margin where the pattern still
   // excites, and the z slab. fovCellX/Y: grid cell of the FOV corner,
   // fracX/Y: its sub-cell offset in [0, 1).
   static WidefieldGridSpec GridSpecFor(const IlluminationPattern& pattern, const WidefieldSceneSpec& spec,
                                        unsigned* fovCellX = nullptr, unsigned* fovCellY = nullptr,
                                        double* fracX = nullptr, double* fracY = nullptr);

   const WidefieldGridSpec& Grid() const { return rect_; }
   double AxisXUm() const { return axisX_; }
   double AxisYUm() const { return axisY_; }
   // Emitted photons per unbleached dye in one frame, per grid column.
   const std::vector<float>& FrameDose() const { return dD_; }
   long Dyes() const { return dyes_.nBleaching + dyes_.nPersistent; }
   long BleachingDyes() const { return dyes_.nBleaching; }
   long ClampedDyes() const { return clamped_; }
   unsigned FftSize() const { return std::max(nx_, ny_); }
   unsigned FftSizeX() const { return nx_; }
   unsigned FftSizeY() const { return ny_; }
   int KernelRadius() const { return R_; }
   int PsfPlanes() const { return static_cast<int>(kernels_.size()); }
   // Dye planes of the current focus per resolution level (full, 1/2, 1/4).
   const unsigned* PlanesPerLevel() const { return planesPerLevel_; }
   size_t CachedSpectraBytes() const { return cacheBytes_; }

   // Per-column camera photons per bleaching dye for a frame: fresh sample
   // (D0 = framesBefore x dD) or from a dose map (BleachField::DoseOver).
   void FreshBleachWeights(double framesBefore, std::vector<float>& wb) const;
   void BleachWeightsFromDose(const std::vector<float>& d0, std::vector<float>& wb) const;

   // Anchors the bleaching basis at wb (spectra and images of every basis
   // map for the current focus).
   void SetBleachWeights(const std::vector<float>& wb);
   // True (and a) if wb is in the anchored basis: wb = sum_j a_j phi_j.
   bool BleachCoefficients(const std::vector<float>& wb, std::vector<double>& a) const;
   // The coefficients of the anchor weights themselves.
   std::vector<double> AnchorCoefficients() const;
   // Kept for one-map bases (a uniform frame dose): wb = c x the anchor.
   bool ScalarOfBleaching(const std::vector<float>& wb, double& c) const;

   // The images of the current focus (thread-safe to read and Render from).
   const WidefieldImages& Images() const { return images_; }
   // Adds the frame with bleach coefficients a to cam (width x height).
   void RenderCoefficients(const std::vector<double>& a, std::vector<float>& cam) const { images_.Render(a, cam); }
   // One frame: the basis coefficients when wb fits, else SetBleachWeights
   // first. Adds to cam.
   void RenderFrame(const std::vector<float>& wb, std::vector<float>& cam);
   bool LastFrameFast() const { return lastFast_; }

   // Images at other focus positions (world z of the focal plane) for the
   // same pose, weights and basis -- a z series. Computed in parallel over
   // the positions; the scene's own focus is unchanged.
   bool FocusSeries(const std::vector<double>& focusWorldUm, std::vector<WidefieldImages>& out, std::string& err);
   // Changes whenever the images of any focus would change other than by the
   // focus itself (pose, weights, bleach basis, PSF): FocusSeries results
   // stay valid while it does not.
   unsigned long long ImagesVersion() const { return imagesVersion_; }
   // Moves the scene to a focus whose images FocusSeries made at `version`
   // (no convolution); false if the version is stale.
   bool AdoptFocus(double focusWorldUm, const WidefieldImages& images, unsigned long long version);

   // GPU mode: power-of-two FFT sizes and no coarse bands (what the GPU
   // kernels do). Set before the first Update.
   void SetGpuMode(bool on) { gpuMode_ = on; }
   // A synchronous GPU host for the focus work (plane spectra, re-pairing,
   // images); nullptr = the CPU. On a host failure the scene keeps rendering
   // on the CPU and GpuError() says why.
   void SetAccelerator(WidefieldAccelerator* acc) { accel_ = acc; }
   bool UsingAccelerator() const { return accel_ != nullptr; }
   const std::string& GpuError() const { return gpuError_; }
   // The GPU job of the current focus (false if it cannot run on the GPU:
   // not GPU mode, or an FFT beyond kMaxGpuFft); the images it yields go to
   // SetImages (one per job channel, in order).
   bool MakeGpuJob(WidefieldGpuJob& job) const;
   // Deferred images: Update / SetBleachWeights plan the focus but leave the
   // images to SetImages (a GPU host) or ComputeCpuImages.
   void SetDeferImages(bool on) { defer_ = on; }
   void ComputeCpuImages();
   bool SetImages(std::vector<std::vector<float>>& images);

   static constexpr double kBandEpsilon = 1e-6;
   // Tests only: a looser band criterion, to exercise the coarse levels with
   // a PSF that (rightly) never meets kBandEpsilon.
   void SetBandEpsilonForTesting(double e) { bandEpsilon_ = e; }
   static constexpr unsigned kMaxGroups = 8;
   static constexpr unsigned kChebTerms = 12;
   static constexpr int kLevels = 3;

private:
   struct KernelSpec
   {
      int level = 0; // coarsest usable level
      std::vector<cfloat> spec[kLevels];
   };
   struct Channel
   {
      int pop = 1;                // 0 bleaching, 1 persistent
      std::vector<float> weight;  // per grid cell
      unsigned long long version = 0;
   };
   struct FocusPlan
   {
      struct Dep { long k; int p0; float w0, w1; int level; };
      std::vector<Dep> deps;
      long clamped = 0;
      unsigned perLevel[kLevels] = {0, 0, 0};
   };

   bool Finish(const WidefieldSceneSpec& spec, const IlluminationPattern& pattern, const WidefieldPsf& psf,
               bool rectChanged, std::string& err);
   void SetupFft(const WidefieldPsf& psf, double kernelCapUm, bool keepRadius);
   void MakeKernel(const WidefieldPsf& psf, int p, KernelSpec& ks) const;
   FocusPlan PlanFocus(const WidefieldPsf& psf, double focusWorldUm);
   // Makes sure the plane spectra the plans need exist for the channels.
   void FillSpectra(const std::vector<const Channel*>& chans, const std::vector<const FocusPlan*>& plans);
   const std::vector<cfloat>* CachedSpectrum(const Channel& c, long k, int level) const;
   void PlaneSpectrum(const Channel& c, const WidefieldSparsePlane& pl, int level, std::vector<cfloat>& out) const;
   // Full-resolution spectrum of a channel at a plan, then its image.
   void ChannelImage(const Channel& c, const FocusPlan& plan, std::vector<float>& img) const;
   void ImagesFor(const FocusPlan& plan, WidefieldImages& out) const;
   std::vector<const Channel*> ActiveChannels() const;
   void Refocus(const WidefieldPsf& psf);
   void DropChannel(const Channel& c);
   void BuildBleachChannels(const std::vector<float>& wb);

   std::shared_ptr<WidefieldDyeTiles> tiles_;
   WidefieldSceneSpec spec_;
   bool haveSpec_ = false;
   WidefieldGridSpec rect_;
   WidefieldDyePlanes dyes_;
   const WidefieldPsf* psf_ = nullptr;
   unsigned fovX0_ = 0, fovY0_ = 0;
   double fracX_ = 0.0, fracY_ = 0.0;
   double axisX_ = 0.0, axisY_ = 0.0;
   std::vector<float> illum_, dD_, wp_;
   // FFT sizes (levels: n / 1, 2, 4), kernel radius.
   unsigned nx_ = 0, ny_ = 0;
   int R_ = 0;
   int levels_ = 1;
   RealFft2d fft_[kLevels];
   std::map<int, KernelSpec> kernels_;
   long kernelsVersion_ = -1;
   // Channels: [0] persistent (may be unused), [1..] bleach basis maps.
   Channel persistent_;
   std::vector<Channel> bleach_;
   // Bleach basis: groups (distinct frame dose values) or Chebyshev in dD.
   bool bleachValid_ = false, cheb_ = false;
   std::vector<float> groupDose_;
   float doseMax_ = 0.0f;
   std::vector<float> wbRef_;
   // Plane spectra cache: (channel version, plane k, level) -> spectrum.
   struct CacheEntry
   {
      std::vector<cfloat> spec;
      unsigned long long used = 0;
   };
   std::map<std::tuple<unsigned long long, long, int>, CacheEntry> cache_;
   size_t cacheBytes_ = 0, cacheBudget_ = 0;
   mutable unsigned long long cacheClock_ = 0;
   FocusPlan plan_;
   WidefieldImages images_;
   long clamped_ = 0;
   unsigned planesPerLevel_[kLevels] = {0, 0, 0};
   bool lastFast_ = false;
   double bandEpsilon_ = kBandEpsilon;
   unsigned long long imagesVersion_ = 1;
   bool gpuMode_ = false, defer_ = false;
   WidefieldAccelerator* accel_ = nullptr;
   std::string gpuError_;
   unsigned long long geometry_ = 0;
   bool JobFor(const FocusPlan& plan, const std::vector<const Channel*>& chans, WidefieldGpuJob& job) const;
   // Images of the plan's channels on the accelerator; false (accelerator
   // dropped, gpuError_ set) on a failure.
   bool AccelImages(const FocusPlan& plan, const std::vector<const Channel*>& chans,
                    std::vector<std::vector<float>>& out);
};

} // namespace sim
