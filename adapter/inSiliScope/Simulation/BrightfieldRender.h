///////////////////////////////////////////////////////////////////////////////
// FILE:          BrightfieldRender.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The BrightField modality: transmitted-light images of the
//                CellField world (spec/BRIGHTFIELD.md). Scalar wave optics,
//                partially coherent Koehler illumination by Abbe source-
//                point integration, the specimen by multislice (beam)
//                propagation through refractive-index slices built from the
//                core's optical volume (isc_optical_volume_in_window:
//                cytoplasm, nucleus, microtubules -- only what the world
//                simulates). One slice is the thin-object (projection)
//                model. The light runs down (-z) through the cells to the
//                coverslip; each source's exit field spectrum is cached, so
//                a focus change is a pupil + defocus multiply and one inverse
//                FFT per source.
//
//                Determinism: every source's image is a pure function of its
//                index, binned to camera pixels in its own slot and summed in
//                source order, so nothing depends on the thread count.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Drift.h"
#include "Fft2d.h"
#include "SMLMZernike.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class CellFieldSource;

// The speed/precision trade-off as one number: level 1 (fastest) .. 4 (most
// precise) are exposed; 3 is the default. Level 5 exists as a reference for
// checks (cli/viewer/MM clamp to 4). A spec field left at 0 takes the level's value.
struct BrightfieldQuality
{
   int sources = 0;      // condenser source points
   int upscale = 0;      // grid cells per camera pixel
   int sub = 0;          // geometry samples per grid cell side
   double sliceUm = 0.0; // multislice step; 0 = one thin slice
   double marginUm = 0.0;
};
BrightfieldQuality BrightfieldQualityLevel(int level);

struct BrightfieldSpec
{
   // FOV: world um of the top-left corner, camera pixels, pixel size.
   double originXUm = 0.0, originYUm = 0.0;
   unsigned width = 0, height = 0;
   double pixelUm = 0.1;
   // Speed/precision (BrightfieldQualityLevel); 0 fields follow `quality`.
   int quality = 3;
   int sources = 0, upscale = 0, sub = 0;
   double sliceUm = -1.0; // < 0: from quality; 0: thin (one slice)
   double marginUm = 0.0; // 0: from quality
   // Optics.
   double wavelengthNm = 550.0;
   double na = 1.4;           // objective (detection) NA
   double condenserNa = 0.55; // illumination NA (0 = coherent, on axis)
   ZernikeCoefficients zernike = {}; // detection pupil aberrations, waves
   // Specimen: refractive indices and absorption (1/um of cell material, on
   // intensity).
   double nMedium = 1.337, nCytoplasm = 1.35, nNucleus = 1.35, nMicrotubule = 1.48;
   double absorptionPerUm = 0.0;

   // The values after the quality level fills the 0 fields.
   BrightfieldQuality Resolved() const;
};

class BrightfieldScene
{
public:
   // Builds the slices and the per-source exit fields for spec (world
   // `worldVersion` of src). A repeat with the same spec and version is free.
   bool Update(CellFieldSource& src, const BrightfieldSpec& spec, uint64_t worldVersion, std::string& err)
   {
      return Update(src, spec, worldVersion, false, err);
   }
   // deferSources: the per-source propagation (the exit fields) is left to
   // Image / SourceImageAt, which compute it on demand (the viewer's worker
   // split, spec/PORT.md 15: another scene gets the phase screens through
   // UpdateFromPhase and computes some of the sources).
   bool Update(CellFieldSource& src, const BrightfieldSpec& spec, uint64_t worldVersion, bool deferSources,
               std::string& err);
   // The same scene from given phase slices instead of the world (the checks,
   // and the worker split): phase[k * GridNx * GridNy + i] = k0 dz dn of slice
   // k (k = 0 lowest), dz = zTopUm / slices; atten the amplitude factor per
   // element (empty: 1); objectZUm the height of the lowest screen (Update's
   // ObjectZUm: the phase-weighted mean for a thin screen); grid size as
   // Update would choose (GridFor).
   bool UpdateFromPhase(const BrightfieldSpec& spec, int slices, double zTopUm, const std::vector<float>& phase,
                        std::string& err)
   {
      return UpdateFromPhase(spec, slices, zTopUm, 0.5 * zTopUm / std::max(1, slices), phase, std::vector<float>(),
                             false, err);
   }
   bool UpdateFromPhase(const BrightfieldSpec& spec, int slices, double zTopUm, double objectZUm,
                        const std::vector<float>& phase, const std::vector<float>& atten, bool deferSources,
                        std::string& err);
   // The scene's screens, for another scene's UpdateFromPhase.
   const std::vector<float>& Phase() const { return phase_; }
   const std::vector<float>& Atten() const { return atten_; }
   double ZTopUm() const { return dz_ * slices_; }
   // One source's transmitted intensity per camera pixel at focusUm (its
   // exit field computed on demand); SetImageFromSources forms the image of
   // all sources (Sources() x width x height floats, source-major) exactly
   // as Image() does (the same sum in source order), and caches it for
   // Image(focusUm).
   bool SourceImageAt(double focusUm, int s, std::vector<float>& out, std::string& err);
   bool SetImageFromSources(double focusUm, const float* slots);
   bool HasImage(double focusUm) const { return valid_ && haveImage_ && imageFocus_ == focusUm; }
   static void GridFor(const BrightfieldSpec& spec, unsigned& nx, unsigned& ny, unsigned& marginCells);
   // Grid cells per camera pixel: the quality's upscale, raised until the
   // pitch is <= lambda / (4 n_medium) (the propagating field and the
   // intensity are then sampled without aliasing).
   static unsigned UpscaleFor(const BrightfieldSpec& spec);

   // Transmitted intensity per camera pixel (1 = the empty field) with the
   // focal plane at focusUm (um above the coverslip). Cached per focus.
   bool Image(double focusUm, std::vector<float>& out, std::string& err);
   // The transmitted intensity on the whole grid (margins included; mean of
   // the sources, summed in source order), as its spectrum (FineFft()'s
   // layout): periodic and band-limited (pitch <= lambda / 4n), so a drifting
   // sample's frame is this spectrum shifted (BrightfieldDriftFrames).
   bool FineSpectrum(double focusUm, std::vector<cfloat>& spec, std::string& err);
   const RealFft2d& FineFft() const { return fineFft_; }
   unsigned Upscale() const { return up_; }
   unsigned MarginCells() const { return margin_; }
   double PitchUm() const { return pitch_; }
   // Changes whenever the scene is rebuilt.
   uint64_t Version() const { return version_; }

   const BrightfieldSpec& Spec() const { return spec_; }
   bool Valid() const { return valid_; }
   unsigned GridNx() const { return nx_; }
   unsigned GridNy() const { return ny_; }
   int Slices() const { return slices_; }
   double ObjectZUm() const { return objectZ_; }
   int Sources() const { return static_cast<int>(src_.size()); }
   double SetupMs() const { return setupMs_; }
   double LastImageMs() const { return imageMs_; }

private:
   struct Source { int mx, my; }; // tilt on the grid: k = 2 pi (mx / Lx, my / Ly)
   // 2D complex FFTs, cache-blocked (16 rows / columns at a time). The
   // propagating field lives in |k| < k0 n_medium: `band` transforms only the
   // columns that can hold it (the forward transform leaves the others
   // undefined; the inverse expects them zero). The inverse writes rows
   // [row0, row1) only (the rest undefined), normalised.
   void FftForward(cfloat* a, std::vector<cfloat>& work, bool band) const;
   void FftInverse(cfloat* a, std::vector<cfloat>& work, unsigned row0, unsigned row1) const;
   void FftRows(cfloat* a, std::vector<cfloat>& work, unsigned row0, unsigned row1, bool conjIn, float outScale,
                bool conjOut) const;
   void FftCols(cfloat* a, std::vector<cfloat>& work, const std::vector<unsigned>& cols, bool conjIn,
                bool conjOut) const;
   void ExitField(const Source& s, std::vector<cfloat>& u, std::vector<cfloat>& work) const;
   bool Begin(const BrightfieldSpec& spec, std::string& err);
   void Finish(bool deferSources);
   void Defocus(double focusUm, std::vector<cfloat>& defocus) const;
   void EnsureExitFields();   // the deferred per-source propagation (parallel over the sources)
   void SourceImage(int s, const std::vector<cfloat>& defocus, std::vector<cfloat>& u, std::vector<cfloat>& work,
                    float* camOut) const;
   // Source s's field spectrum at the focal plane (pupil and defocus applied).
   void SourceFocalSpectrum(int s, const std::vector<cfloat>& defocus, std::vector<cfloat>& u,
                            std::vector<cfloat>& work) const;

   BrightfieldSpec spec_;
   uint64_t worldVersion_ = 0;
   const void* srcId_ = nullptr;
   bool valid_ = false;
   unsigned nx_ = 0, ny_ = 0, up_ = 1, margin_ = 0;
   double pitch_ = 0.1, k0_ = 0.0, dz_ = 0.0;
   double objectZ_ = 0.0; // height of the lowest screen (thin: phase-weighted mean height)
   int slices_ = 1;
   FftPlan1d planX_, planY_;
   std::vector<float> phase_, atten_; // per slice: k0 dz dn, and the amplitude factor (empty: 1)
   std::vector<Source> src_;
   std::vector<cfloat> pupil_;        // detection pupil (aperture + aberration), per k
   std::vector<float> kz_;            // axial wavenumber in the medium (< 0: evanescent)
   std::vector<cfloat> prop_;         // one slice step dz: exp(i kz dz), 0 where evanescent
   std::vector<cfloat> trans_;        // per slice transmittance exp(i phase) * amplitude (empty: on the fly)
   std::vector<unsigned> allCols_, bandCols_; // FFT columns: all, and those with |kx| < k0 n_medium
   std::vector<cfloat> thinSpec_;     // one slice: the transmittance spectrum (every source shifts it)
   std::vector<std::vector<cfloat>> exit_; // multislice: per source exit spectrum (empty: recompute)
   bool haveImage_ = false;
   double imageFocus_ = 0.0;
   std::vector<float> image_;
   double setupMs_ = 0.0, imageMs_ = 0.0;
   RealFft2d fineFft_;
   uint64_t version_ = 0;
};

// The frames of a drifting sample (Drift.h): the scene's fine-grid intensity
// spectra on the drift's focus grid around each base focus (made on demand,
// remade when the scene is rebuilt), each frame interpolated in z and
// shifted in xy by a phase ramp, cropped and binned to camera pixels. The
// scene's margin must exceed the xy drift.
class BrightfieldDriftFrames
{
public:
   void Begin(const DriftBounds& b, const std::vector<double>& baseFocusUm);
   // The spectra the frames of base focus `base` need; false with err.
   bool Refresh(BrightfieldScene& scene, size_t base, std::string& err);
   // Only the (one or two) foci a frame with z drift dzNm needs (live mode).
   bool Ensure(BrightfieldScene& scene, size_t base, double dzNm, std::string& err);
   // Transmitted intensity per camera pixel (1 = the empty field) of the
   // frame with drift d (into out, W x H). Thread-safe after Refresh.
   void Image(const BrightfieldScene& scene, size_t base, const DriftNm& d, std::vector<float>& out) const;

private:
   DriftFocusGrid grid_;
   std::vector<double> base_;
   std::vector<std::vector<cfloat>> spec_; // base-major, grid_.n per base (empty: not made)
   uint64_t version_ = 0;
};

} // namespace sim
