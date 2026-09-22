#include "SMLMBackground.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;

int Mirror(int v, int n)
{
   // Repeated reflection, so a blur radius larger than the image still
   // lands inside it.
   if (n <= 1)
      return 0;
   const int period = 2 * n - 2;
   v %= period;
   if (v < 0)
      v += period;
   return v < n ? v : period - v;
}
} // namespace

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

std::vector<float> GaussianBlur(const std::vector<float>& src, unsigned width, unsigned height, double sigma)
{
   const int w = static_cast<int>(width), h = static_cast<int>(height);
   const int rad = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
   std::vector<double> k(static_cast<size_t>(2 * rad + 1));
   double s = 0.0;
   for (int i = -rad; i <= rad; ++i)
   {
      k[static_cast<size_t>(i + rad)] = std::exp(-i * i / (2.0 * sigma * sigma));
      s += k[static_cast<size_t>(i + rad)];
   }
   for (double& v : k)
      v /= s;

   std::vector<float> tmp(src.size()), dst(src.size());
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
         double a = 0.0;
         for (int i = -rad; i <= rad; ++i)
            a += k[static_cast<size_t>(i + rad)] * src[static_cast<size_t>(y) * w + Mirror(x + i, w)];
         tmp[static_cast<size_t>(y) * w + x] = static_cast<float>(a);
      }
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
         double a = 0.0;
         for (int i = -rad; i <= rad; ++i)
            a += k[static_cast<size_t>(i + rad)] * tmp[static_cast<size_t>(Mirror(y + i, h)) * w + x];
         dst[static_cast<size_t>(y) * w + x] = static_cast<float>(a);
      }
   return dst;
}

std::vector<float> BuildBackgroundMap(unsigned width, unsigned height, double meanBackground, double cellContrast,
                                      double hazeWeight, double hazeSigmaPx,
                                      const std::vector<std::pair<double, double>>& sitesPx, std::mt19937_64& rng)
{
   if (!(meanBackground > 0.0) || (!(cellContrast > 1.0) && !(hazeWeight > 0.0)) || width == 0 || height == 0)
      return {};
   const size_t n = static_cast<size_t>(width) * height;
   std::vector<float> f(n, 1.0f);
   std::uniform_real_distribution<double> unif(0.0, 1.0);

   if (cellContrast > 1.0)
   {
      const double cx = width * (0.5 + 0.12 * (unif(rng) - 0.5));
      const double cy = height * (0.5 + 0.12 * (unif(rng) - 0.5));
      const double a = width * (0.34 + 0.08 * unif(rng));
      const double b = height * (0.24 + 0.08 * unif(rng));
      const double th = unif(rng) * kPi, c = std::cos(th), s = std::sin(th);
      for (unsigned y = 0; y < height; ++y)
         for (unsigned x = 0; x < width; ++x)
         {
            const double u = ((x - cx) * c + (y - cy) * s) / a, v = (-(x - cx) * s + (y - cy) * c) / b;
            // Superellipse radius (1 = the cell outline), soft edge ~6% of the cell size.
            const double r = std::cbrt(std::pow(std::fabs(u), 3.0) + std::pow(std::fabs(v), 3.0));
            f[static_cast<size_t>(y) * width + x] = static_cast<float>(1.0 + (cellContrast - 1.0) / (1.0 + std::exp((r - 1.0) / 0.06)));
         }
   }

   if (hazeWeight > 0.0 && !sitesPx.empty())
   {
      std::vector<float> d(n, 0.0f);
      for (const auto& p : sitesPx)
      {
         const long x = std::lround(p.first), y = std::lround(p.second);
         if (x >= 0 && y >= 0 && x < static_cast<long>(width) && y < static_cast<long>(height))
            d[static_cast<size_t>(y) * width + static_cast<size_t>(x)] += 1.0f;
      }
      std::vector<float> hz = GaussianBlur(d, width, height, std::max(0.1, hazeSigmaPx));
      double m = 0.0;
      for (float v : hz)
         m += v;
      m /= static_cast<double>(n);
      if (m > 0.0)
         for (size_t i = 0; i < n; ++i)
            f[i] = static_cast<float>(f[i] + hazeWeight * hz[i] / m);
   }

   double mean = 0.0;
   for (float v : f)
      mean += v;
   mean /= static_cast<double>(n);
   for (float& v : f)
      v = static_cast<float>(v * meanBackground / mean);
   return f;
}

double BackgroundFadeScale(double t, double decay)
{
   return decay > 0.0 ? 0.3 + 0.7 * std::exp(-t / decay) : 1.0;
}

} // namespace sim
