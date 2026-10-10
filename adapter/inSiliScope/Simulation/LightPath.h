///////////////////////////////////////////////////////////////////////////////
// FILE:          LightPath.h
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   The microscope's light path (issue 16): laser lines at the
//                sample, the excitation (laser clean-up) filter, which scales
//                each line by its transmission there, the dichroic (reflects the lasers, R = 1 - T;
//                transmits the emission), the emission filter, the camera's
//                QE curve, the objective's collection efficiency and the
//                imaging chamber's height (DNA-PAINT imager background). The
//                twin of makeLightPath / backgroundQe in
//                web/prototype/scope/dye_library.js.
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#pragma once

#include "Spectra.h"

#include <string>
#include <vector>

namespace sim {

struct LaserLine
{
   double nm, kWPerCm2;   // intensity at the sample with a perfect mirror (in a LightPath: x the excitation filter)
};

struct LightPath
{
   std::vector<LaserLine> lasers;   // only those with kWPerCm2 > 0, in option order, x the excitation filter's T
   std::string excitationFilterName, dichroicName, emissionFilterName, qeName;
   Spectrum excitationT, dichroicT, emissionT, qe;
   double na = 1.4, immersionIndex = 1.518, chamberHeightUm = 5.0;
   double eta = 0.0;                // CollectionEfficiency(na, immersionIndex)
};

struct LightPathSettings
{
   std::vector<LaserLine> lasers;   // every line (0 = off)
   int excitationFilter = 0;        // index into ExcitationFilterIds()
   double exLoNm = 635, exHiNm = 645;   // the Custom excitation band pass
   int dichroic = 0;                // index into DichroicIds()
   double dichroicEdgeNm = 650;     // the Custom dichroic's long-pass edge
   int emissionFilter = 0;          // index into EmissionFilterIds()
   double emLoNm = 657.5, emHiNm = 694.5;   // the Custom band pass
   int qeCurve = 0;                 // index into CameraIds(); a "Flat" camera: qeFlat everywhere
   double qeFlat = 0.85;
   double na = 1.4, immersionIndex = 1.518, chamberHeightUm = 5.0;
};

// False (with err) on an index out of range.
bool MakeLightPath(const LightPathSettings& s, LightPath& out, std::string& err);

// Laser intensity reaching the sample (x the dichroic's reflectance) of the
// lines in [loNm, hiNm] (primed conversion windows), kW/cm^2.
double LaserIntensityIn(const LightPath& lp, double loNm, double hiNm);
// ... of the lines within 0.5 nm of nm.
double LaserIntensityAt(const LightPath& lp, double nm);

// QE at the emission filter's (x dichroic) transmission-weighted centre:
// the flat background's QE.
double BackgroundQe(const LightPath& lp);

} // namespace sim
