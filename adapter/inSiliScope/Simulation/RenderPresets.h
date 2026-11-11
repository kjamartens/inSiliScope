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
// (psf-oversampling) and the kernel's coarsest wanted sample spacing
// (psf-max-sample-nm: Fast 30, Realistic 15 nm, Exhaustive none; the user's
// choice 2026-11-03), mean-field upscaling (wf-upscale), the blink halo cut
// (psf-halo-cut), the dimmest blink splatted in a frame (blink-min-photons,
// detected photons; an *estimate* of what a localization needs) and whether a
// clump of fiducial beads is drawn as one bead of their volume
// (fid-clumps-as-one; Exhaustive: each bead, the user's choice 2026-10-30) and
// whether a structured pattern lights the blink modes' continuous dyes frame by
// frame (sim-frame-pattern-continuous; else the cycle mean: Exhaustive only, the
// user's choice 2026-10-31) and the mean-field dye plane thickness (wf-plane-nm:
// Fast and Realistic 100 nm, Exhaustive 25 nm; the user's choice 2026-11-11: live SIM2D
// WideField 256 px 2.3-2.6x faster a frame, the noise-free photons 0.25-0.9 % rms from
// 25 nm's in epi, HILO, SIM2D/3D, a 40 nm pixel (2.5 % at most on bright pixels),
// against a shot noise of 7-40 %; TIRF caps it, ScopeMovie.cpp kTirfPlanesPerDecay)
// per level.
struct QualityPreset
{
   const char* name;
   double bf;
   int os;
   double maxSampleNm;
   double wf;
   double halo;
   double minPhotons;
   double clumpsAsOne;
   double framePattern;
   double wfPlaneNm;
};
constexpr QualityPreset kQualityPresets[] = {
   { "Fast", 1, 4, 30, 1, 1e-5, 100, 1, 0, 100 },
   { "Realistic", 3, 6, 15, 1, 3e-6, 50, 1, 0, 100 },
   { "Exhaustive", 4, 8, 0, 2, 0.0, 0, 0, 1, 25 },
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
