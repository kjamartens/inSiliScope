///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMZernike.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Parse/format helpers and named presets for the 28-value
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

// Number of coefficients: OSA/ANSI single index 0-27, every mode up to 6th
// radial order (n <= 6) -- matches webSMLM's PSF_NZERNIKE and
// GibsonLanniZernikePSF.N_ZERNIKE.
constexpr size_t kNumZernike = 28;

// Index = OSA/ANSI single Zernike index, j = n(n+1)/2 + (l+n)/2:
//   n=0..4 (0-14): 0 piston, 1 tip, 2 tilt, 3 oblique astigmatism,
//     4 defocus, 5 vertical astigmatism, 6 oblique trefoil, 7 vertical coma,
//     8 horizontal coma, 9 vertical trefoil, 10 oblique quadrafoil,
//     11 oblique secondary astigmatism, 12 primary spherical, 13 vertical
//     secondary astigmatism, 14 vertical quadrafoil;
//   n=5 (15-20): l = -5,-3,-1,+1,+3,+5 (pentafoil, secondary trefoil,
//     secondary coma, each oblique then vertical);
//   n=6 (21-27): l = -6,-4,-2,0,+2,+4,+6 -- 24 is secondary spherical, 25
//     vertical tertiary astigmatism.
// Values are in waves (a coefficient of 1.0 means one full wave of
// unnormalized peak Zernike amplitude -- see GibsonLanniZernikePSF.java's
// class Javadoc for the exact convention). webSMLM's UI takes milliwaves;
// this project's property stays in waves.
using ZernikeCoefficients = std::array<double, kNumZernike>;

// Round-trip helpers for the PsfZernikeCoefficients property, positional
// rather than token-skipping (mirroring
// FormatResolutionSpacingsNm/ParseResolutionSpacingsNm in style --
// SMLMPatterns.h -- but positional: index = array position = OSA mode
// number). Values may be separated by spaces, commas or semicolons: MMCore
// rejects a comma in any property value SET through it
// (MM::g_FieldDelimiters), so the property itself is space-separated
// (FormatZernikeCoefficients' default), while the JVM bridge is handed a
// comma-separated copy. ParseZernikeCoefficients accepts EXACTLY 15 or 28
// valid numbers -- a 15-value list (the pre-28-mode format) is zero-
// padded, the same rule as webSMLM's custom-coefficient field -- and for
// anything else logs nothing itself (callers -- SMLMImageGeneration.cpp's
// OnPsfZernikeCoefficients -- decide how to react) but returns all-zero
// (unaberrated) rather than partially applying a misaligned list, since a
// wrong count would otherwise silently assign a coefficient to the wrong
// Zernike mode. FormatZernikeCoefficients always writes all 28.
std::string FormatZernikeCoefficients(const ZernikeCoefficients& coeffs, char separator = ' ');
ZernikeCoefficients ParseZernikeCoefficients(const std::string& text, bool& outOk);

// All-zero (unaberrated) coefficients.
ZernikeCoefficients ZeroZernikeCoefficients();

// Named aberration templates for the PsfZernikePreset property. Values are
// webSMLM's own PSF_ZERNIKE_PRESETS (webSMLM.html), under this project's
// preset names -- ported for parity, so both simulators render the same
// PSF for the same preset:
//   - the single-mode and "MixedRealisticObjective" presets are order-of-
//     magnitude wavefront-error estimates (~0.07 waves = the Marechal/
//     diffraction-limit criterion, lambda/14; ~0.1-0.15 mild; ~0.25-0.3
//     strong), NOT numbers sourced from a specific paper -- see
//     docs/vectorial-psf-step4-smlm-challenge-comparison.md's "gap to flag
//     explicitly" note;
//   - SaddlePoint/ExtendedRange/ExtendedRangeStrong are engineered
//     astigmatic PSFs for a longer single-valued z range, stacking
//     astigmatism at OSA j=5/13/25 (webSMLM measured, at 3000 photons /
//     5 bg/px / 100 nm px: z range +/-700/+/-1000/+/-1300 nm vs +/-500 nm
//     for AstigmatismModerate, at the cost of ~2.3x z-CRLB at focus). They
//     are deliberately not called tetrapods -- no four-lobe shape.
// Returns ZeroZernikeCoefficients() for an unrecognized name (including
// "None").
ZernikeCoefficients ZernikePresetCoefficients(const std::string& presetName);

// Every recognized preset name, in property-registration order (index 0 is
// "None").
const std::vector<std::string>& ZernikePresetNames();

} // namespace sim
