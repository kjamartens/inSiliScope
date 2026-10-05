///////////////////////////////////////////////////////////////////////////////
// FILE:          Spectra.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Spectra.h (twin of web/prototype/scope/spectra.js).
//
// LICENSE:       BSD-3-Clause (see LICENSE at the repository root)

#include "Spectra.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim {

namespace {
constexpr double kPlanck = 6.62607015e-34, kLightSpeed = 299792458;
}

double SampleAt(const double* values, double nm)
{
   const double x = (nm - kSpectraGridMinNm) / kSpectraGridStepNm;
   if (!(x >= 0) || x > kSpectraGridN - 1)
      return 0;
   const int i = static_cast<int>(std::min(static_cast<double>(kSpectraGridN - 2), std::floor(x)));
   const double f = x - i;
   return values[i] * (1 - f) + values[i + 1] * f;
}

Spectrum SkewedGaussian(double peakNm, double leftWidthNm, double rightWidthNm)
{
   Spectrum out(kSpectraGridN);
   const double k = 4 * std::log(2.0);
   for (int i = 0; i < kSpectraGridN; ++i)
   {
      const double d = SpectraGridNm(i) - peakNm, w = d < 0 ? leftWidthNm : rightWidthNm;
      out[static_cast<size_t>(i)] = std::exp(-k * std::pow(d / std::max(1e-6, w), 2));
   }
   return out;
}

Spectrum ParametricExcitation(double peakNm, double widthNm)
{
   return SkewedGaussian(peakNm, 1.6 * widthNm, 0.6 * widthNm);
}

Spectrum ParametricEmission(double peakNm, double widthNm)
{
   return SkewedGaussian(peakNm, 0.6 * widthNm, 1.6 * widthNm);
}

Spectrum IdealTransmission(const IdealFilterSpec& spec)
{
   Spectrum out(kSpectraGridN);
   for (int i = 0; i < kSpectraGridN; ++i)
   {
      const double nm = SpectraGridNm(i);
      double t = 1;
      switch (spec.type)
      {
      case FilterType::LongPass: t = nm >= spec.edgeNm ? 1 : 0; break;
      case FilterType::ShortPass: t = nm <= spec.edgeNm ? 1 : 0; break;
      case FilterType::BandPass: t = nm >= spec.loNm && nm <= spec.hiNm ? 1 : 0; break;
      case FilterType::Notch:
         for (int b = 0; b < spec.nBands; ++b)
            if (nm >= spec.bands[b][0] && nm <= spec.bands[b][1])
               t = 0;
         break;
      case FilterType::None: break;
      }
      out[static_cast<size_t>(i)] = t;
   }
   return out;
}

double CrossSectionUm2(double epsPeak, const Spectrum& ex, double nm)
{
   return std::log(10.0) * 1000.0 * epsPeak * SampleAt(ex, nm) / kAvogadro * 1e8;
}

double PhotonFluxPerUm2(double kWPerCm2, double nm)
{
   return kWPerCm2 * 1e3 / (kPlanck * kLightSpeed / (nm * 1e-9)) * 1e-8;
}

double CollectionEfficiency(double na, double n)
{
   const double r = std::min(1.0, std::max(0.0, na / std::max(1e-6, n)));
   return 0.5 * (1.0 - std::sqrt(1.0 - r * r));
}

Detection Detect(const Spectrum& em, const std::vector<const Spectrum*>& curves)
{
   double total = 0, kept = 0, moment = 0;
   for (int i = 0; i < kSpectraGridN; ++i)
   {
      const double e = em[static_cast<size_t>(i)];
      if (!(e > 0))
         continue;
      double t = 1;
      for (const Spectrum* c : curves)
         t *= (*c)[static_cast<size_t>(i)];
      total += e;
      kept += e * t;
      moment += e * t * SpectraGridNm(i);
   }
   return { total > 0 ? kept / total : 0,
            kept > 0 ? moment / kept : std::numeric_limits<double>::quiet_NaN() };
}

} // namespace sim
