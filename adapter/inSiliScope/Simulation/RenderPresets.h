///////////////////////////////////////////////////////////////////////////////
// FILE:          RenderPresets.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The preset tables a Micro-Manager Basic property sets in one
//                go, kept here once: Renderer.Quality (Registry/PropertyTable.cpp)
//                and SampleHolder.DriftPreset (Registry/Couplings.cpp). The cli
//                prints them (--presets-json) for the docs' comparison figures
//                (tools/build_physics_figures.py), so the figures show what the
//                adapter applies.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

namespace sim {

// Renderer.Quality: BrightField quality level (bf-quality), PSF oversampling
// (psf-oversampling), mean-field upscaling (wf-upscale), the blink halo cut
// (psf-halo-cut), the dimmest blink splatted in a frame (blink-min-photons,
// detected photons; an *estimate* of what a localization needs) and whether a
// clump of fiducial beads is drawn as one bead of their volume
// (fid-clumps-as-one; Exhaustive: each bead, the user's choice 2026-10-30) per
// level.
struct QualityPreset
{
   const char* name;
   double bf;
   int os;
   double wf;
   double halo;
   double minPhotons;
   double clumpsAsOne;
};
constexpr QualityPreset kQualityPresets[] = {
   { "Fast", 1, 4, 1, 1e-5, 100, 1 },
   { "Realistic", 3, 6, 1, 3e-6, 50, 1 },
   { "Exhaustive", 4, 8, 2, 0.0, 0, 0 },
};

// SampleHolder.DriftPreset (Off .. Extreme): xy and z drift speed (nm/s) and
// xy and z random walk (nm / sqrt s); tiers chosen by the user, *estimate*
// (Ma et al. 2024's 5-20 nm/s RMS drifts, refs ma2024, sit between Medium and
// High).
struct DriftPreset
{
   const char* name;
   double speedNmPerSec;
   double walkNmPerSqrtSec;
};
constexpr DriftPreset kDriftPresets[] = {
   { "Off", 0.0, 0.0 },
   { "Low", 2.0, 0.4 },
   { "Medium", 5.0, 1.0 },
   { "High", 25.0, 5.0 },
   { "Extreme", 250.0, 50.0 },
};
constexpr int kDriftPresetCount = static_cast<int>(sizeof kDriftPresets / sizeof kDriftPresets[0]);

} // namespace sim
