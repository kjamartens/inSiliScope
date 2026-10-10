///////////////////////////////////////////////////////////////////////////////
// FILE:          SceneSettings.h
// PROJECT:       insiliscope
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   The settings (SceneState.h) as the engine's structures: the
//                scope spec FluorescenceMovie renders (the cli/viewer's
//                options), the noise/timing parameters, the PSF request, the
//                cell field and the BrightField spec. Pure functions of the
//                settings, so any device (the camera's render threads, a
//                readout on another device) can build them.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "SceneState.h"

#include "../Simulation/BrightfieldRender.h"
#include "../Simulation/CellFieldSource.h"
#include "../Simulation/Drift.h"
#include "../Simulation/ScopeMovie.h"
#include "../Simulation/SMLMBackground.h"
#include "../Simulation/SMLMSimulation.h"

#include <string>

namespace isc {

// Dyes up to this far outside the FOV are still rendered: their PSF tails
// reach in (spec/PORT.md 6.2; a few kernel half-widths of the in-focus core).
constexpr double kCellFieldMarginUm = 2.0;

// The imaging the open light sources give: 0 Fluorescence (the epi light,
// alone or with the lamp), 1 BrightField (the transmitted lamp alone).
int Modality(const SceneState& s);
inline bool BrightFieldSelected(const SceneState& s) { return Modality(s) == 1; }
// Any light source open (none: dark frames).
inline bool LightOn(const SceneState& s) { return s.epiOpen.load() || s.transOpen.load(); }
// The light of a frame: -1 none (dark), 0 epi (fluorescence), 1 the lamp
// (BrightField), 2 both (fluorescence + the lamp's photons x its QE, one
// noise chain; the engine's light-epi + light-trans).
enum LightMode { LIGHT_NONE = -1, LIGHT_EPI = 0, LIGHT_TRANS = 1, LIGHT_BOTH = 2 };
inline int LightModeOf(const SceneState& s)
{
   const bool e = s.epiOpen.load(), t = s.transOpen.load();
   return e && t ? LIGHT_BOTH : e ? LIGHT_EPI : t ? LIGHT_TRANS : LIGHT_NONE;
}

// Noise chain and timing: the per-second rates at the current exposure.
sim::SimulationParams SnapshotParams(const SceneState& s);
// What ComputePsfKernelCache needs for the diffraction kernel (the emission
// wavelength is a placeholder; the engine's hook sets it per dye state).
sim::PsfGeneratorRequest BuildPsfGeneratorRequest(const SceneState& s);
// The illumination field of a w x h frame (empty when flat); meanFactor: its mean.
sim::StackShapingFields BuildShapingFields(const SceneState& s, unsigned w, unsigned h, double* meanFactor);
// The cell field's world (seed, geometry, cache directory).
sim::CellFieldSettings BuildCellFieldSettings(const SceneState& s);
// A warning when the diffraction kernel's z range cannot cover the CellField
// z slab (dyes beyond it are drawn on the kernel's end plane); empty otherwise.
std::string CellFieldZRangeWarning(const SceneState& s);
// The query for a w x h FOV centred on the stage (x, y) with the Z stage at
// zStageUm over simulated [tSec, tSec + spanSec): the FOV shifted against the
// drift at the start and end of that span (drift px) plus a PSF margin.
sim::CellFieldQuery CellFieldQueryFor(const SceneState& s, double stageX, double stageY, double zStageUm, unsigned w,
                                      unsigned h, const sim::SimulationParams& params, double drift0XPx,
                                      double drift0YPx, double drift1XPx, double drift1YPx, long frameIndex,
                                      double tSec, double spanSec);
// The QE of the camera's curve at the BrightField lamp wavelength.
double BrightFieldQe(const SceneState& s);
// The BrightField spec at the pose of q (FOV origin), w x h pixels.
sim::BrightfieldSpec BuildBrightfieldSpec(const SceneState& s, const sim::SimulationParams& params,
                                          const sim::CellFieldQuery& q, unsigned w, unsigned h);
// The scope spec for a FOV centred on the stage (x, y) with the Z stage at z,
// from simulated time startSec, frames long.
sim::ScopeSpec BuildScopeSpec(const SceneState& s, double stageXUm, double stageYUm, double zStageUm, double startSec,
                              long frames);
// The sample drift as the engine's movie options (ScopeMovie: drift-*).
void AddDriftToSpec(sim::ScopeSpec& spec, const sim::DriftSettings& d);
// The spec as text (ParseScopeSpec's format: name=value, one per line).
std::string ScopeSpecText(const sim::ScopeSpec& spec);

} // namespace isc
