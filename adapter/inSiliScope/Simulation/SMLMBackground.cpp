#include "SMLMBackground.h"

#include <algorithm>
#include <cmath>

namespace sim {

std::vector<float> BuildIlluminationField(unsigned width, unsigned height, IllumProfile profile, double fwhmPct,
                                          double* outMeanFactor)
{
   if (outMeanFactor)
      *outMeanFactor = 1.0;
   if (profile == IllumProfile::Flat || width == 0 || height == 0)
      return {};
   const double cx = (width - 1) / 2.0, cy = (height - 1) / 2.0;
   const double fwhm = std::max(1e-6, (fwhmPct / 100.0) * width);
   const double sig = fwhm / 2.3548, r0 = fwhm / 2.0, edge = std::max(1e-6, 0.15 * r0);
   std::vector<float> f(static_cast<size_t>(width) * height);
   double sum = 0.0, peak = 0.0;
   for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x)
      {
         const double r = std::hypot(x - cx, y - cy);
         const double v = profile == IllumProfile::Gaussian ? std::exp(-(r * r) / (2.0 * sig * sig))
                                                            : 1.0 / (1.0 + std::exp((r - r0) / edge));
         f[static_cast<size_t>(y) * width + x] = static_cast<float>(v);
         sum += v;
         peak = std::max(peak, v);
      }
   if (!(peak > 0.0))
      return {};
   for (float& v : f)
      v = static_cast<float>(v / peak);
   if (outMeanFactor)
      *outMeanFactor = sum / (static_cast<double>(width) * height) / peak;
   return f;
}

double IlluminationAt(const std::vector<float>& field, unsigned width, unsigned height, double xPx, double yPx)
{
   if (field.empty() || field.size() != static_cast<size_t>(width) * height)
      return 1.0;
   const double xi = std::max(0.0, std::min(static_cast<double>(width - 1), xPx));
   const double yi = std::max(0.0, std::min(static_cast<double>(height - 1), yPx));
   const unsigned x0 = std::min(width - 1, static_cast<unsigned>(std::floor(xi)));
   const unsigned y0 = std::min(height - 1, static_cast<unsigned>(std::floor(yi)));
   const unsigned x1 = std::min(width - 1, x0 + 1), y1 = std::min(height - 1, y0 + 1);
   const double fx = xi - x0, fy = yi - y0;
   auto at = [&](unsigned x, unsigned y) { return static_cast<double>(field[static_cast<size_t>(y) * width + x]); };
   return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
}

double BackgroundFadeScale(double t, double decay)
{
   return decay > 0.0 ? 0.3 + 0.7 * std::exp(-t / decay) : 1.0;
}

} // namespace sim
