///////////////////////////////////////////////////////////////////////////////
// FILE:          ZernikePsf.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The GibsonLanniZernike PSF in C++: a scalar Gibson-Lanni
//                pupil (sample-depth and immersion-defocus OPD terms, unit
//                amplitude, no apodization or polarization) plus a Zernike
//                phase (OSA/ANSI 0-27, waves) and an optional double-helix
//                phase mask, evaluated by a separable chirp-Z transform on a
//                64 x 64 Cartesian pupil grid. A port of webSMLM's
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

#include <string>
#include <vector>

namespace sim {

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

} // namespace sim
