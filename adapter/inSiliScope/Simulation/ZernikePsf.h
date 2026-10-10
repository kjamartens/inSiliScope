///////////////////////////////////////////////////////////////////////////////
// FILE:          ZernikePsf.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The GibsonLanniZernike PSF in C++: a scalar Gibson-Lanni
//                pupil (sample-depth and immersion-defocus OPD terms, unit
//                amplitude, no apodization or polarization) plus a Zernike
//                phase (OSA/ANSI 0-27, waves) and an optional double-helix
//                phase mask, evaluated by a separable chirp-Z transform on an
//                M x M Cartesian pupil grid (ZernikePupilSamples; webSMLM
//                and the JVM class use 64). A port of webSMLM's
//                computePsfPupilCartesianForZPlane /
//                computePsfIntensityPlaneFFT / psfCzt1d (the copy in
//                tools/psf_parity_check/dump_websmlm.mjs) and of
//                psfbridge/GibsonLanniZernikePSF.java, which computes the
//                same thing in the JVM and stays as the reference. Same plane
//                grid as the JVM path for the same PsfGeneratorRequest.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "PsfGeneratorBridge.h"
#include "SMLMZernike.h"

#include <string>
#include <vector>

namespace sim {

// Pupil samples per axis for req: req.pupilSamples when > 0, else the
// smallest multiple of 4 (64-512) whose repeat distance (M - 4) lambda /
// (2 NA) is the window's diagonal (light can fold into the window only from
// beyond it), then raised to fill the chirp-Z transform's power-of-two
// length (free; it shrinks the pupil-edge staircase error too). A sampled
// pupil makes the PSF periodic: with 64 samples (the old value, webSMLM's)
// the period was ~13.7 um in the red, ~10 um in the green, less than the
// 14 um wide default window, so folded light showed. The default window
// gets 160 samples, the viewer's 3 um one 140, at no extra cost; against
// 768 samples a camera pixel is then off by <= 1.3e-5 of an emitter's
// photons beyond 2 um (was 1.5e-4) and <= 9e-5 in the core (was 2.9e-3)
// (spec/ALGORITHM.md). JS twin web/prototype/scope/psf.js pupilSamples.
int ZernikePupilSamples(const PsfGeneratorRequest& req);

// Raw (unnormalized) intensity planes for req (req.model is not read):
// out[k] has size*size floats, row-major (x fastest), size =
// 2*kernelHalfWidthPx*oversampling + 1 at pixelSizeNm/oversampling; nz =
// max(3, req.nz) made odd, plane k at defocus (k - (nz-1)/2) * zStepNm
// (k = 0 lowest). Planes run on all cores (serially under Emscripten).
bool ComputeZernikePsfPlanes(const PsfGeneratorRequest& req, std::vector<std::vector<float>>& out,
                             std::string& outError);

// The same planes as a PsfKernelCache, filled exactly as the JVM path fills
// it (sum-1 planes, block sums). Not memoized: ComputePsfKernelCache is the
// memoized entry point for every model.
bool BuildZernikePsfKernelCache(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError);

// Wavefront of the Zernike coefficients (OSA/ANSI, waves) at pupil radius
// rho (0..1) and angle phi, in waves (0 outside the pupil). The same modes
// the PSF uses; the BrightField detection pupil takes it.
double ZernikeWavefrontWaves(const ZernikeCoefficients& coeffs, double rho, double phi);

// The nonzero terms of a coefficient set with their (n, l, m) resolved once,
// for many evaluations of the same wavefront (BrightField's detection pupil):
// Waves(rho, phi) == ZernikeWavefrontWaves(coeffs, rho, phi), the same
// operations in the same order.
class ZernikeModeTable
{
public:
   explicit ZernikeModeTable(const ZernikeCoefficients& coeffs);
   double Waves(double rho, double phi) const;

private:
   struct Term { double coeff; int n, l, m; };
   std::vector<Term> terms_;
};

} // namespace sim
