// The cell field the engine's checks were written on: the core's default field until 2026-11-27 (occupancy 0.33,
// jitter 0.8, cells of 25-35 um, elongation 0.5-1, blobbiness 1.75, roughness 0.15, fractal dimension 1.35). The
// checks aim at places of that field (a window beside a nucleus, a stretch of microtubules, a cell edge), so they pin
// it; the defaults are the "Round, smooth" cell at occupancy 0.7 and jitter 1 since (the user's choice).
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "insiliscope/insiliscope.h"

#include <string>
#include <utility>
#include <vector>

namespace testfield {

// Core parameter names and values.
inline const std::vector<std::pair<std::string, double>>& Params()
{
   static const std::vector<std::pair<std::string, double>> p = {
      { "density", 0.33 },     { "jitter", 0.8 },       { "cellDiamMin", 25 },  { "cellDiamMax", 35 },    { "cellElongMin", 0.5 },
      { "cellElongMax", 1 },   { "cellBlob", 1.75 },    { "cellRough", 0.15 },  { "cellFractalDim", 1.35 },
   };
   return p;
}

// ... set on a core parameter object.
inline void Set(IscParams* p)
{
   for (const auto& kv : Params())
      isc_params_set(p, kv.first.c_str(), kv.second);
}

// ... as engine spec options, to put before a check's own options (a later option wins).
inline const char* Spec()
{
   return "occupancy=0.33 cell-diam-min-um=25 cell-diam-max-um=35 p.jitter=0.8 p.cellElongMin=0.5 p.cellElongMax=1 "
          "p.cellBlob=1.75 p.cellRough=0.15 p.cellFractalDim=1.35 ";
}

} // namespace testfield
