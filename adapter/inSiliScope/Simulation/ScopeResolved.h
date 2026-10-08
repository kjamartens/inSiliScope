///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeResolved.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Read-only views of what a ScopeMovie spec resolves to and
//                renders, for diagnostics (the cli's docs-figure outputs,
//                cli/scope_probes.*; tools/build_physics_figures.py): the
//                setup a movie builds (camera, light path, labels, world
//                settings, FOV query) and each frame's photon image before
//                the camera. Neither changes what a movie renders.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "ScopeMovie.h"

#include "CellFieldSource.h"
#include "Drift.h"
#include "DyeLibrary.h"
#include "LightPath.h"
#include "SMLMSimulation.h"

#include <functional>
#include <string>
#include <vector>

namespace sim {

// The camera a spec resolves to (its preset, with the options given on top).
struct ScopeResolvedCamera
{
   std::string preset;
   int qeCurve = 0;
   double qeFlat = 0.85;
   bool emccd = false;
   double darkPerSec = 0, gainElectronsPerAdu = 1, offsetAdu = 100, offsetStdAdu = 0, readNoiseElectrons = 0;
   double gainStdFraction = 0, readNoiseStdFraction = 0, emGain = 300, cicElectrons = 0, bitDepth = 16;
};

// What a movie of the spec is set up with (ScopeMovie.cpp's MakeScopeSetup,
// the JS scopeSetup): frame size and count, exposure, start time, the camera,
// the noise chain's parameters (p: QE 1 for fluorescence, whose detected
// fractions hold the QE curve; BrightField: the QE at the lamp wavelength),
// the light path, each structure's label physics, the world and the FOV query,
// and the drift per frame (empty: none).
struct ScopeResolved
{
   unsigned W = 0, H = 0;
   long N = 0, seed = 0;
   double expSec = 0, t0Sec = 0;
   ScopeResolvedCamera camera;
   SimulationParams p;
   LightPath lp;
   std::vector<LabelPhysics> labels;
   CellFieldSettings cf;
   CellFieldQuery q;
   std::vector<DriftNm> drift;
};
bool ScopeResolve(const ScopeSpec& spec, ScopeResolved& out, std::string& err);

// The movie RenderScopeMovie makes, but each frame's photon image before the
// camera goes to onPhotons (return false to stop) instead of the noise chain:
// fluorescence (detected photons; the noise chain runs at QE 1), BrightField
// (the lamp's photons at the camera, before its QE), both lights (fluorescence
// + lamp x the QE at the lamp wavelength, as the one noise chain gets them) or
// none (zeros). The same frames, in the same order, from the same objects.
using ScopePhotonSink = std::function<bool(long f, const std::vector<float>& photons)>;
bool RenderScopePhotons(const ScopeSpec& spec, const ScopePhotonSink& onPhotons, ScopeMovieInfo& info,
                        std::string& err);

} // namespace sim
