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
   double nMedium = 1.337, nCytoplasm = 1.360, nNucleus = 1.355, nMicrotubule = 1.48;
   double absorptionPerUm = 0.0;

   // The values after the quality level fills the 0 fields.
   BrightfieldQuality Resolved() const;
};

class BrightfieldScene
{
public:
   // Builds the slices and the per-source exit fields for spec (world
   // `worldVersion` of src). A repeat with the same spec and version is free.
   bool Update(CellFieldSource& src, const BrightfieldSpec& spec, uint64_t worldVersion, std::string& err);
   // Test hook: the same scene from given phase slices instead of the world:
   // phase[k * GridNx * GridNy + i] = k0 dz dn of slice k (k = 0 lowest),
   // dz = zTopUm / slices; grid size as Update would choose (GridFor).
   bool UpdateFromPhase(const BrightfieldSpec& spec, int slices, double zTopUm, const std::vector<float>& phase,
                        std::string& err);
   static void GridFor(const BrightfieldSpec& spec, unsigned& nx, unsigned& ny, unsigned& marginCells);

   // Transmitted intensity per camera pixel (1 = the empty field) with the
   // focal plane at focusUm (um above the coverslip). Cached per focus.
   bool Image(double focusUm, std::vector<float>& out, std::string& err);

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
   void Fft2(cfloat* a, std::vector<cfloat>& work, bool inverse) const;
   void ExitField(const Source& s, std::vector<cfloat>& u, std::vector<cfloat>& work) const;
   bool Begin(const BrightfieldSpec& spec, std::string& err);
   void Finish();
   void SourceImage(int s, double focusUm, std::vector<cfloat>& u, std::vector<cfloat>& work, float* camOut) const;

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
   std::vector<cfloat> thinSpec_;     // one slice: the transmittance spectrum (every source shifts it)
   std::vector<std::vector<cfloat>> exit_; // multislice: per source exit spectrum (empty: recompute)
   bool haveImage_ = false;
   double imageFocus_ = 0.0;
   std::vector<float> image_;
   double setupMs_ = 0.0, imageMs_ = 0.0;
};

} // namespace sim
