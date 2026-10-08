// scope_probes -- insiliscope_cli's diagnostic outputs: read-only views of what
// a movie of the spec renders (the adapter's Simulation/ code, ScopeResolved.h),
// for the docs' physics figures (tools/build_physics_figures.py). None changes
// the movie. Each writes its file(s) and returns false with err on a failure.
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include "ScopeMovie.h"

#include <string>

namespace probes {

// The resolved setup as JSON: frame, camera, noise-chain parameters, light path
// curves (on the spectra grid), each structure's label and its states (spectra,
// detected spectrum, rates, detected fraction, PSF wavelength), the PSF request
// of the main state and the resolved BrightField settings.
bool SetupJson(const sim::ScopeSpec& spec, const std::string& path, std::string& err);

// The preset tables (RenderPresets.h: Renderer.Quality, SampleHolder.DriftPreset)
// and the Zernike presets, as JSON.
bool PresetsJson(const std::string& path, std::string& err);

// Each frame's photon image before the camera (RenderScopePhotons) as a float32
// TIFF, one page per frame.
bool Photons(const sim::ScopeSpec& spec, const std::string& path, sim::ScopeMovieInfo& info, std::string& err);

// The movie's PSF (MakeScopePsfPreview): <prefix>.planes.tif (oversampled
// planes, sum 1 each), <prefix>.cams.tif (camera images of one 1-photon
// emitter per plane), <prefix>.pupil.tif (the Zernike wavefront in waves on a
// grid over the pupil, NaN outside) and <prefix>.json.
bool Psf(const sim::ScopeSpec& spec, const std::string& prefix, std::string& err);

// One blink of 1 photon as a movie draws it (RenderPhotonImage with the main
// state's kernel and the spec's halo cut and interpolation), at (dx, dy) pixels
// from the centre pixel's centre and zUm above the focal plane: a float32 TIFF
// over the kernel's square, and <path>.json (sum, kept pixels).
bool Splat(const sim::ScopeSpec& spec, double dxPx, double dyPx, double zUm, const std::string& path,
           std::string& err);

// The dyes of the spec's world and labels in [x0, x1) x [y0, y1) (world um) and
// [zMin, zMax): sites (stride 5), blink events in [t0, t1) (stride 10) and the
// continuous windows ending after t0 (stride 10), as JSON.
bool DyesJson(const sim::ScopeSpec& spec, const double rect[4], double zMin, double zMax, double t0, double t1,
              const std::string& path, std::string& err);

// Dye counts per z plane over the FOV (isc_density3d_in_window, as the
// mean-field scenes bin them): nz planes in [zMin, zMax), up cells per pixel,
// a float32 TIFF and <path>.json.
bool Density(const sim::ScopeSpec& spec, int nz, double zMin, double zMax, int up, const std::string& path,
             std::string& err);

// The BrightField scene's phase (k0 dz dn) and attenuation screens per slice
// (BrightfieldMovie): <prefix>.phase.tif, <prefix>.atten.tif (if any) and
// <prefix>.json (grid, slices, heights).
bool BrightfieldScreens(const sim::ScopeSpec& spec, const std::string& prefix, std::string& err);

// The shaped nucleus surface of each cell in the square of side sizeUm around
// the spec's x, y (isc_cell_nucleus_rings, world um), as JSON.
bool NucleusJson(const sim::ScopeSpec& spec, double sizeUm, const std::string& path, std::string& err);

} // namespace probes
