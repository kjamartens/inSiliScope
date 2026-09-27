///////////////////////////////////////////////////////////////////////////////
// FILE:          Illumination.cpp
// PROJECT:       insiliscope
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See Illumination.h.
//
// LICENSE:       BSD (see license.txt)

#include "Illumination.h"

#include <cstddef>

namespace sim {

void IlluminationPattern::Sample(double x0Um, double y0Um, double pitchUm, unsigned nx, unsigned ny,
                                 float* out) const
{
   for (unsigned iy = 0; iy < ny; ++iy)
   {
      const double y = y0Um + (iy + 0.5) * pitchUm;
      for (unsigned ix = 0; ix < nx; ++ix)
         out[static_cast<size_t>(iy) * nx + ix] = static_cast<float>(At(x0Um + (ix + 0.5) * pitchUm, y));
   }
}

double SquareIllumination::At(double dxUm, double dyUm) const
{
   return (dxUm >= -w_ / 2 && dxUm < w_ / 2 && dyUm >= -h_ / 2 && dyUm < h_ / 2) ? 1.0 : 0.0;
}

void SquareIllumination::Support(double& x0, double& y0, double& x1, double& y1) const
{
   x0 = -w_ / 2;
   x1 = w_ / 2;
   y0 = -h_ / 2;
   y1 = h_ / 2;
}

} // namespace sim
