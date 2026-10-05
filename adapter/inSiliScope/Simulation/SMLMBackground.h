///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMBackground.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Spatial fields that shape the simulated light, ported from the
//                webSMLM reference simulator (webSMLM.html -- see PARITY.md in
//                that project): the excitation illumination profile
//                (buildSimIllumination/illumAt) and the background's fade
//                over time (simBgScale).
//
//                Every builder returns an EMPTY vector for its "off" setting;
//                callers read that as "multiply by 1" / "flat scalar
//                background", which is what keeps default output byte-
//                identical to before these features existed.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include <vector>

namespace sim {

enum class IllumProfile
{
   Flat = 0,
   Gaussian = 1,
   // A flat-topped disc with a soft (logistic) edge -- the shape the SMLM
   // Challenge 2016 simulator used. webSMLM's 'sigmoid' (UI label
   // "Flat-top").
   FlatTop = 2,
};

// Illumination field over the camera grid (row-major, width*height), as a
// multiplier with PEAK 1 -- an attenuation, never a gain, so photon and
// background settings are the values at the beam centre (how they are quoted
// at a microscope). FWHM is fwhmPct percent of the FOV WIDTH, for both
// profiles; FlatTop's disc radius is FWHM/2 with an edge width of 15% of
// that radius. Empty for Flat. outMeanFactor (optional) receives the field's
// mean, i.e. the fraction of the total photon budget the profile keeps, for
// logging.
std::vector<float> BuildIlluminationField(unsigned width, unsigned height, IllumProfile profile, double fwhmPct,
                                          double* outMeanFactor = nullptr);

// Bilinear read of an illumination field at a sub-pixel camera position,
// clamped to the field. 1 for an empty field.
double IlluminationAt(const std::vector<float>& field, unsigned width, unsigned height, double xPx, double yPx);

// Background fade over time: autofluorescence bleaches too, fast at first
// and then onto a 30% floor -- 0.3 + 0.7*exp(-t/decay). 1 (no fade) when decay <= 0. t and decay in the
// same unit (demoCam passes seconds).
double BackgroundFadeScale(double t, double decay);

} // namespace sim
