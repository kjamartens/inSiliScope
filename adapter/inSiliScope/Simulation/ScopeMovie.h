///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeMovie.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   A short cell-field movie without Micro-Manager, for one FOV:
//                the twin of web/prototype/scope/scope_movie.js +
//                fluorescence.js (the JS reference, issue 16). Fluorescence
//                (modality 0): every structure's label in its mode
//                (dSTORM, PALM, DNA-PAINT, WideField) through the light path
//                (DyeLibrary.h, LightPath.h): blinks per (structure, state)
//                group with that group's PSF kernel (one per detected
//                wavelength, rounded to 2 nm), continuous populations
//                (WideField dyes, PALM pre states, the dSTORM initial ON)
//                mean-field (WidefieldRender.h) or per dye (a running
//                image), the DNA-PAINT free imager as a static offset, then
//                ApplyNoiseChain at QE 1. BrightField (modality 1):
//                BrightfieldRender.h. Used by cli/ (TIFF files) and the
//                viewer (web/, via the WASM export isc_scope_movie). PSF:
//                GibsonLanniZernike (C++ ZernikePsf.h; psf-* options = the
//                PSFParam_ properties) or Gaussian (psf-model 0);
//                RichardsWolf/GibsonLanni need the adapter's JVM.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sim {

struct ScopeOption
{
   const char* name;
   double value;       // default
   const char* help;
};

// Every named option with its default (the adapter's property defaults).
const std::vector<ScopeOption>& ScopeMovieOptions();

// Option values by name; missing ones take their default. Besides the named
// options, "p.<name>" passes a core world parameter through (the prototype's
// names, e.g. p.mtWobbleTurn); unknown p.* names are ignored. "zern.<j>"
// (j = 0..27) sets Zernike coefficient j in waves, replacing the preset's.
// "<prefix>-dye.<field>" (prefix mt) and "dye<N>.<field>" override a field
// of a structure's dye or of slot N (DyeFieldNames()).
using ScopeSpec = std::map<std::string, double>;

// False for a name that is neither a named option nor p.* / zern.*.
bool ScopeSpecSet(ScopeSpec& spec, const std::string& name, double value);
double ScopeSpecGet(const ScopeSpec& spec, const char* name);
// An option's value from text: a number, or a name (modality: Fluorescence,
// BrightField; psf-model: Gaussian, GibsonLanniZernike, ...; psf-mask: None,
// DoubleHelix; psf-interp: Nearest, Linear, Cubic, Fft; psf-zernike-preset:
// the PSFParam_PsfZernikePreset names; mt-dye, mt-mode, dye<N>.source,
// dichroic, em-filter, light-preset, camera-preset, qe-curve, camera-type:
// their data/dyes ids). False if neither.
bool ScopeOptionValue(const std::string& name, const char* text, double& value);
// "k=v k=v ..." (spaces, commas or semicolons); false (with err) on a bad token.
bool ParseScopeSpec(const std::string& text, ScopeSpec& spec, std::string& err);

struct ScopeMovieInfo
{
   unsigned width = 0, height = 0;
   long frames = 0;
   size_t blinks = 0;
   long dyes = 0;            // dyes of the continuous populations in the z range
   double halfTimeSec = 0;   // unused since issue 16 (infinity)
   double querySec = 0, totalSec = 0;
   std::string description;   // one line of the settings, for file metadata
   std::vector<double> driftNm; // sample drift: x, y, z per frame (empty: none)
};

struct PsfGeneratorRequest;
struct PsfKernelCache;
// The spec's PSF at an emission wavelength as the camera's
// BuildPsfGeneratorRequest would build it from the same PSFParam_ values.
// False for psf-model 0 (Gaussian; err empty) or a bad value (err set).
bool ScopePsfRequest(const ScopeSpec& spec, double wavelengthNm, PsfGeneratorRequest& req, std::string& err);
// Its kernel (memoized, ComputePsfKernelCache); cache.valid = false and true
// returned for the Gaussian.
bool ScopePsfKernel(const ScopeSpec& spec, double wavelengthNm, PsfKernelCache& cache, std::string& err);
// Hosts with the PSFGenerator JVM (the adapter) build psf-model 1/2
// (RichardsWolf, GibsonLanni) requests themselves: the hook gets the spec and
// the wavelength and fills req (false: an error). Unset: those models are
// rejected.
using ScopePsfHook = std::function<bool(const ScopeSpec& spec, double wavelengthNm, PsfGeneratorRequest& req)>;
void SetScopePsfRequestHook(ScopePsfHook hook);

// The detection of structure s's dye state (pre: the PALM pre state) in the
// spec's light path: detected fraction, PSF wavelength (nm), detected photons
// per second while emitting, and the ON time now (dSTORM: scaled by the
// excitation). False (with err) on a bad spec; emits = false for a dark state.
struct ScopeStateReadout
{
   bool emits = false;
   double detectedFraction = 0, lambdaNm = 0, detectedPerSec = 0, onSecNow = 0;
};
bool ScopeLabelState(const ScopeSpec& spec, int structure, bool pre, ScopeStateReadout& out, std::string& err);

// A PSF wavelength rounded to 2 nm (< 0.3 % in PSF width), so small light-path
// or dye changes reuse a kernel (JS kernelWavelengthNm).
double KernelWavelengthNm(double lambdaNm);

// The PSF a movie of the spec uses, for display (the viewer's Preview PSF): the
// microtubules' emitting state's kernel (KernelWavelengthNm of its detected
// wavelength; the pre state when the main one is dark) and, per z plane, the
// camera image (camSize^2) of one emitter of 1 photon at the centre of the
// middle pixel, splatted as a movie does (PlanSplat/SplatRows with the spec's
// interpolation). Gaussian (psf-model 0): one plane, the Gaussian sampled at
// psf-oversampling, and its RenderGaussianPSF image. JS scopePsfPreview.
struct ScopePsfPreview
{
   bool gaussian = false;
   int oversampling = 1, size = 0, nz = 0, camSize = 0;
   double zStepNm = 0, lambdaNm = 0;
   std::vector<float> planes, cams;   // nz * size^2 (sum 1 each), nz * camSize^2
};
bool MakeScopePsfPreview(const ScopeSpec& spec, ScopePsfPreview& out, std::string& err);

// The cell geometry of the spec's world (the same world the movie renders)
// in the square of side sizeUm centred on the spec's x, y, as JSON in world
// um: {"x0","y0","x1","y1","cells":[{"x","y","height","outline":[[x,y]...],
// "nucleus":{"x","y","rot","long","short","z","height"}, with detail also
// "mesh":{"rings","n","v":[[x,y,h]...]}, "mts":[[[x,y,z]...]...]}]}. The
// cell-local geometry is rotated by packRot and moved to the cell's x, y
// (the viewer's localToWorld). Image row 0 of a movie is the smallest y.
bool ScopeGeometryJson(const ScopeSpec& spec, double sizeUm, bool detail, std::string& json, std::string& err);

// Frame size and count of a spec (no rendering).
void ScopeMovieDims(const ScopeSpec& spec, unsigned& w, unsigned& h, long& frames);

// Progress of a movie, per frame: ("frames", fraction done, a JSON object
// {"frame","frames","blinks","backends":[...]} naming which backend drew each
// part of the frame: the SMLM splat, mean-field (FFT) or per dye).
using ScopeProgress = std::function<void(const char* stage, double frac, const std::string& detailJson)>;

class WidefieldScene;

// A fluorescence movie in steps: Begin (the world, the groups' kernels, the
// blinks, the continuous populations and the mean-field scenes of those that
// start mean-field), then -- in GPU mode, images deferred -- each mean-field
// scene's images from a GPU host (MeanFieldScene(i).MakeGpuJob,
// SetMeanFieldImages: the viewer's WebGPU) or ComputeCpuImages(), then Render
// (background, blinks, continuous populations, noise per frame).
// RenderScopeMovie runs the steps on the CPU. Begin holds the movie cache's
// lock until the object is destroyed.
class WidefieldAccelerator;
struct PsfKernelCache;
struct BlinkEvent;

// What a host (the Micro-Manager adapter) adds per frame on top of the JS
// reference's movie, all off by default (= the JS): the focal plane per frame
// (a z sequence), a drift of the blinks (px), the illumination field (W x H,
// peak 1: multiplies the background, the imager offset and each blink at its
// site) and a background fade factor. Continuous populations are neither
// drifted nor shaped by the field.
struct FluorescenceFrameOptions
{
   std::function<double(long f)> zStageUm;              // empty: the spec's z
   std::function<void(long f, double& dxPx, double& dyPx)> driftPx;
   const std::vector<float>* illumField = nullptr;
   std::function<double(long f)> backgroundScale;       // empty: 1
   // Set: the photon images (before the camera) go here instead of the noise
   // chain and onFrame (the host adds its own noise).
   std::function<bool(long f, const std::vector<float>& photons)> onPhotons;
};

// A host's per-region clock (the adapter's illumination history): the
// seconds of illumination each world position (um) has had before frame 0. A
// dye's schedule is read at its position's clock instead of start-sec; frame
// f adds f x exposure everywhere (the whole query rect is lit while the movie
// runs). Regions: the distinct clocks in a rect, each with the bounding box
// of the positions that have it.
struct ClockRegion
{
   double tSec = 0, x0Um = 0, y0Um = 0, x1Um = 0, y1Um = 0;
};
class DyeClock
{
public:
   virtual ~DyeClock() = default;
   virtual double At(double xUm, double yUm) const = 0;
   virtual void Regions(double x0Um, double y0Um, double x1Um, double y1Um, std::vector<ClockRegion>& out) const = 0;
};

// The blinks-only case of a movie (one blink group, no continuous
// population): what the adapter's GPU splat + noise path needs.
struct FluorescenceSimplePlan
{
   bool ok = false;
   const PsfKernelCache* kernel = nullptr;   // nullptr: the Gaussian of sigmaPx
   double photonsPerBlink = 0, sigmaPx = 1, backgroundPhotons = 0;
   const std::vector<BlinkEvent>* events = nullptr;
};

class FluorescenceMovie
{
public:
   FluorescenceMovie();
   ~FluorescenceMovie();
   // accel (optional): a synchronous GPU host for the mean-field scenes (the
   // adapter's Direct3D 11 one); their images are then made at once on it.
   // clock (optional): per-region dye clocks instead of start-sec (read
   // during Begin only).
   bool Begin(const ScopeSpec& spec, bool gpuMode, std::string& err, WidefieldAccelerator* accel = nullptr,
              const DyeClock* clock = nullptr);
   int MeanFieldScenes() const;
   WidefieldScene& MeanFieldScene(int i);
   // The images of scene i (one per job channel); false if they do not fit.
   bool SetMeanFieldImages(int i, std::vector<std::vector<float>>& images);
   void ComputeCpuImages();
   FluorescenceSimplePlan SimplePlan() const;
   // Whether the movie has continuous populations (mean-field or per dye).
   bool HasPopulations() const;
   bool Render(const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame, ScopeMovieInfo& info,
               std::string& err, const ScopeProgress* progress = nullptr,
               const FluorescenceFrameOptions* options = nullptr);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

// Warms the shared world's caches around the spec's FOV (CellFieldSource::
// Prefetch over the spec's query at its time, xy margin, at most budgetMs).
bool PrefetchScope(const ScopeSpec& spec, double marginUm, double budgetMs);

// Renders the movie, calling onFrame(f, adu) for f = 0..frames-1 (return
// false to stop). False (with err) on a failure.
bool RenderScopeMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                      ScopeMovieInfo& info, std::string& err, const ScopeProgress* progress = nullptr);

// BrightField (modality 1): the transmitted-light image at the spec's focus
// (BrightfieldScene), times bf-photons-per-px-per-sec x exposure, then the
// camera noise per frame (the specimen does not change between frames).
struct BrightfieldSpec;
bool ScopeBrightfieldSpec(const ScopeSpec& spec, BrightfieldSpec& bs, std::string& err);
bool RenderBrightfieldMovie(const ScopeSpec& spec,
                            const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                            ScopeMovieInfo& info, std::string& err);

// BrightField movie in steps, for the viewer's worker split (spec/PORT.md
// 15): Begin builds the shared world and scene with the per-source
// propagation deferred and exposes the phase screens; a helper's
// BeginFromPhase builds a scene from those screens alone (no world) and
// SourceImage computes one source's camera image; SetSourceImages forms the
// image from all sources' images (Sources() x Width x Height floats) exactly
// as the single-worker path does; Render makes the frames. Begin holds the
// movie cache's lock until the object is destroyed.
class BrightfieldMovie
{
public:
   BrightfieldMovie();
   ~BrightfieldMovie();
   bool Begin(const ScopeSpec& spec, bool deferSources, std::string& err);
   bool BeginFromPhase(const ScopeSpec& spec, int slices, double zTopUm, double objectZUm,
                       const std::vector<float>& phase, const std::vector<float>& atten, std::string& err);
   unsigned Width() const;
   unsigned Height() const;
   long Frames() const;
   int Sources() const;
   int Slices() const;
   unsigned GridNx() const;
   unsigned GridNy() const;
   double ZTopUm() const;
   double ObjectZUm() const;
   const std::vector<float>& Phase() const;
   const std::vector<float>& Atten() const;
   bool SourceImage(int s, std::vector<float>& out, std::string& err);
   bool SetSourceImages(const float* slots);
   bool ImageCached() const;   // the scene already holds the image at the movie's focus (a repeat): no sources to compute
   bool Render(const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame, ScopeMovieInfo& info,
               std::string& err);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace sim
