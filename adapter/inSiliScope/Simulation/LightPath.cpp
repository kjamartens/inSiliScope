///////////////////////////////////////////////////////////////////////////////
// FILE:          LightPath.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See LightPath.h (twin of makeLightPath / backgroundQe in
//                web/prototype/scope/dye_library.js).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "LightPath.h"

#include "DyeLibrary.h"

#include <cmath>
#include <cstring>

namespace sim {

bool MakeLightPath(const LightPathSettings& s, LightPath& out, std::string& err)
{
   out = LightPath();
   auto filter = [&](const FilterData& (*at)(int), size_t n, int i, bool dichroic, std::string& name, Spectrum& T) {
      if (i < 0 || static_cast<size_t>(i) >= n)
      {
         err = "filter index " + std::to_string(i) + " out of range";
         return false;
      }
      const FilterData& f = at(i);
      name = f.name;
      if (f.curve)
      {
         const Spectrum* c = SpectrumByKey(f.curve);
         if (!c)
         {
            err = std::string("no spectrum ") + f.curve;
            return false;
         }
         T = *c;
         return true;
      }
      IdealFilterSpec spec = f.ideal;
      if (!std::strcmp(f.id, "Custom"))
      {
         if (dichroic)
            spec.edgeNm = s.dichroicEdgeNm;
         else
         {
            spec.loNm = s.emLoNm;
            spec.hiNm = s.emHiNm;
         }
      }
      T = IdealTransmission(spec);
      return true;
   };
   if (!filter(DichroicAt, DichroicIds().size(), s.dichroic, true, out.dichroicName, out.dichroicT) ||
       !filter(EmissionFilterAt, EmissionFilterIds().size(), s.emissionFilter, false, out.emissionFilterName,
               out.emissionT))
      return false;
   const CameraData* cam = s.qeCurve >= 0 && static_cast<size_t>(s.qeCurve) < CameraIds().size() ? &CameraAt(s.qeCurve) : nullptr;
   const Spectrum* qe = cam && cam->qeCurve && !std::strncmp(cam->qeCurve, "fp:", 3) ? SpectrumByKey(cam->qeCurve) : nullptr;
   out.qe = qe ? *qe : Spectrum(kSpectraGridN, s.qeFlat);
   out.qeName = cam ? cam->id : "Flat";
   for (const LaserLine& l : s.lasers)
      if (l.kWPerCm2 > 0)
         out.lasers.push_back(l);
   out.na = s.na;
   out.immersionIndex = s.immersionIndex;
   out.chamberHeightUm = s.chamberHeightUm;
   out.eta = CollectionEfficiency(s.na, s.immersionIndex);
   return true;
}

double LaserIntensityIn(const LightPath& lp, double loNm, double hiNm)
{
   double s = 0;
   for (const LaserLine& l : lp.lasers)
      s = s + (l.nm >= loNm && l.nm <= hiNm ? l.kWPerCm2 * (1 - SampleAt(lp.dichroicT, l.nm)) : 0);
   return s;
}

double LaserIntensityAt(const LightPath& lp, double nm)
{
   double s = 0;
   for (const LaserLine& l : lp.lasers)
      s = s + (std::fabs(l.nm - nm) < 0.5 ? l.kWPerCm2 * (1 - SampleAt(lp.dichroicT, l.nm)) : 0);
   return s;
}

double BackgroundQe(const LightPath& lp)
{
   double s = 0, m = 0;
   for (int i = 0; i < kSpectraGridN; ++i)
   {
      const double t = lp.emissionT[static_cast<size_t>(i)] * lp.dichroicT[static_cast<size_t>(i)];
      s += t;
      m += t * SpectraGridNm(i);
   }
   return SampleAt(lp.qe, s > 0 ? m / s : 600);
}

} // namespace sim
