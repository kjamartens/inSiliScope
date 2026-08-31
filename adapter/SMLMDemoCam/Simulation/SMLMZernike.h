///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMZernike.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Parse/format helpers and named presets for the 15-value
//                positional Zernike-coefficient string exposed as the
//                PsfZernikeCoefficients MM property (GibsonLanniZernike PSF
//                model only -- see Simulation/psfbridge-java/psfbridge/
//                GibsonLanniZernikePSF.java and PsfGeneratorBridge.h).
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <array>
#include <string>
#include <vector>

namespace sim {

// Index = OSA/ANSI single Zernike index (0-14, covering every mode up to
// 4th radial order): 0 piston, 1 tip, 2 tilt, 3 oblique astigmatism,
// 4 defocus, 5 vertical astigmatism, 6 oblique trefoil, 7 vertical coma,
// 8 horizontal coma, 9 vertical trefoil, 10 oblique quadrafoil, 11 oblique
// secondary astigmatism, 12 primary spherical, 13 vertical secondary
// astigmatism, 14 vertical quadrafoil. Values are in waves (a coefficient
// of 1.0 means one full wave of unnormalized peak Zernike amplitude -- see
// GibsonLanniZernikePSF.java's class Javadoc for the exact convention).
using ZernikeCoefficients = std::array<double, 15>;

// Comma-separated round-trip helpers for the PsfZernikeCoefficients
// property, positional rather than token-skipping (mirroring
// FormatResolutionSpacingsNm/ParseResolutionSpacingsNm in style --
// SMLMPatterns.h -- but positional: index = array position = OSA mode
// number). ParseZernikeCoefficients requires EXACTLY 15 valid comma-
// separated numbers; a malformed/short/long list logs nothing itself
// (callers -- SMLMImageGeneration.cpp's OnPsfZernikeCoefficients -- decide
// how to react) but returns all-zero (unaberrated) rather than partially
// applying a misaligned list, since a wrong count would otherwise silently
// assign a coefficient to the wrong Zernike mode.
std::string FormatZernikeCoefficients(const ZernikeCoefficients& coeffs);
ZernikeCoefficients ParseZernikeCoefficients(const std::string& text, bool& outOk);

// All-zero (unaberrated) coefficients -- the PsfZernikeCoefficients
// property's default value.
ZernikeCoefficients ZeroZernikeCoefficients();

// Named aberration templates for the PsfZernikePreset property: reasonable,
// order-of-magnitude wavefront-error estimates (in waves) for common
// low-order aberrations seen in real fluorescence-microscopy objectives,
// not numbers sourced from a specific paper -- see the "Zernike coefficient
// values ... gap to flag explicitly rather than paper over with an invented
// number attributed to [a specific paper]" note in
// docs/vectorial-psf-step4-smlm-challenge-comparison.md. Magnitude bands
// follow the general adaptive-optics-microscopy convention that ~0.07 waves
// RMS (the classic Marechal/diffraction-limit criterion, lambda/14) is
// "essentially unaberrated", ~0.1-0.15 waves is a typical mildly aberrated
// objective/mount (matching this project's own pre-existing 0.15-wave
// illustrative astigmatism value in the vectorial-psf-plan.md step 5 test
// plan), and ~0.25-0.3 waves is a visibly/strongly aberrated system.
// Returns ZeroZernikeCoefficients() for an unrecognized name (including
// "None").
ZernikeCoefficients ZernikePresetCoefficients(const std::string& presetName);

// Every recognized preset name, in property-registration order (index 0 is
// the default, "None").
const std::vector<std::string>& ZernikePresetNames();

} // namespace sim
