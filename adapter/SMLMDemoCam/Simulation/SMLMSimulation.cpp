#include "SMLMSimulation.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinLifetimeFrames = 0.01;
} // namespace

void RenderGaussianPSF(std::vector<float>& img, unsigned width, unsigned height,
                        double xPx, double yPx, double sigmaPx, double totalPhotons)
{
   if (totalPhotons <= 0.0 || sigmaPx <= 0.0)
      return;

   int rad = static_cast<int>(std::ceil(3.0 * sigmaPx));
   int cx = static_cast<int>(std::lround(xPx));
   int cy = static_cast<int>(std::lround(yPx));
   double amplitude = totalPhotons / (2.0 * kPi * sigmaPx * sigmaPx);
   double twoSigmaSq = 2.0 * sigmaPx * sigmaPx;

   int yLo = std::max(0, cy - rad);
   int yHi = std::min(static_cast<int>(height) - 1, cy + rad);
   int xLo = std::max(0, cx - rad);
   int xHi = std::min(static_cast<int>(width) - 1, cx + rad);

   for (int py = yLo; py <= yHi; ++py)
   {
      double ddy = py - yPx;
      float* row = img.data() + static_cast<size_t>(py) * width;
      for (int px = xLo; px <= xHi; ++px)
      {
         double ddx = px - xPx;
         double val = amplitude * std::exp(-(ddx * ddx + ddy * ddy) / twoSigmaSq);
         row[px] += static_cast<float>(val);
      }
   }
}

void RenderPhotonImage(std::vector<float>& img, unsigned width, unsigned height,
                        const std::vector<BlinkEvent>& events, long frameIndex,
                        double pixelSizeNm, double psfSigmaPx, double photonsPerBlink,
                        double backgroundPhotons,
                        double driftOffsetXPx, double driftOffsetYPx)
{
   img.assign(static_cast<size_t>(width) * height, static_cast<float>(backgroundPhotons));

   for (const BlinkEvent& e : events)
   {
      double ov = std::min(static_cast<double>(frameIndex + 1), e.tEnd) -
                  std::max(static_cast<double>(frameIndex), e.tStart);
      if (ov <= 0.0)
         continue;
      if (ov > 1.0)
         ov = 1.0;

      double xPx = e.xUm * 1000.0 / pixelSizeNm + driftOffsetXPx;
      double yPx = e.yUm * 1000.0 / pixelSizeNm + driftOffsetYPx;
      RenderGaussianPSF(img, width, height, xPx, yPx, psfSigmaPx, photonsPerBlink * ov);
   }
}

void EmitterModel::SetPattern(std::unique_ptr<IPatternGenerator> pattern)
{
   pattern_ = std::move(pattern);
   liveActive_.clear();
}

void EmitterModel::Reseed(uint64_t seed)
{
   std::mt19937_64 localRng(seed ^ 0x9E3779B97F4A7C15ULL);
   std::uniform_real_distribution<double> angleDist(0.0, 2.0 * kPi);
   driftAngleRad_ = angleDist(localRng);
   driftAngleSet_ = true;
   liveActive_.clear();
}

std::vector<BlinkEvent> EmitterModel::GenerateAllEvents(long nFrames, double widthUm, double heightUm,
                                                          const SimulationParams& params,
                                                          std::mt19937_64& rng) const
{
   std::vector<BlinkEvent> events;
   if (!pattern_ || nFrames <= 0)
      return events;

   double lifetime = std::max(params.onLifetimeFrames, kMinLifetimeFrames);
   double area = widthUm * heightUm;
   double leadIn = 5.0 * lifetime;
   double arrivalRate = params.emitterDensity * area / lifetime;
   double nEventsMean = arrivalRate * (static_cast<double>(nFrames) + leadIn);
   long nEvents = static_cast<long>(PoissonRng(rng, nEventsMean));

   std::uniform_real_distribution<double> tStartDist(-leadIn, static_cast<double>(nFrames));
   std::uniform_real_distribution<double> unif01(0.0, 1.0);

   events.reserve(static_cast<size_t>(nEvents));
   for (long i = 0; i < nEvents; ++i)
   {
      EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
      double tStart = tStartDist(rng);
      double u = std::min(unif01(rng), 0.999999);
      double tEnd = tStart - lifetime * std::log(1.0 - u);
      events.push_back({site.xUm, site.yUm, tStart, tEnd});
   }
   return events;
}

void EmitterModel::ResetLive(double /*widthUm*/, double /*heightUm*/)
{
   liveActive_.clear();
}

std::vector<BlinkEvent> EmitterModel::AdvanceOneFrame(long frameIndex, double widthUm, double heightUm,
                                                        const SimulationParams& params,
                                                        std::mt19937_64& rng)
{
   if (pattern_)
   {
      double lifetime = std::max(params.onLifetimeFrames, kMinLifetimeFrames);
      double area = widthUm * heightUm;
      double arrivalRatePerFrame = params.emitterDensity * area / lifetime;
      long nNew = static_cast<long>(PoissonRng(rng, arrivalRatePerFrame));

      std::uniform_real_distribution<double> unif01(0.0, 1.0);

      for (long i = 0; i < nNew; ++i)
      {
         EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
         double tStart = static_cast<double>(frameIndex) + unif01(rng);
         double u = std::min(unif01(rng), 0.999999);
         double tEnd = tStart - lifetime * std::log(1.0 - u);
         liveActive_.push_back({site.xUm, site.yUm, tStart, tEnd});
      }
   }

   // Retire fully-elapsed events.
   liveActive_.erase(
      std::remove_if(liveActive_.begin(), liveActive_.end(),
                      [frameIndex](const BlinkEvent& e) { return e.tEnd <= static_cast<double>(frameIndex); }),
      liveActive_.end());

   std::vector<BlinkEvent> overlapping;
   overlapping.reserve(liveActive_.size());
   for (const BlinkEvent& e : liveActive_)
   {
      if (e.tStart < static_cast<double>(frameIndex + 1) && e.tEnd > static_cast<double>(frameIndex))
         overlapping.push_back(e);
   }
   return overlapping;
}

void EmitterModel::GetDriftOffsetPx(long frameIndex, long totalFrames, double driftPx,
                                     double& outDx, double& outDy) const
{
   if (driftPx <= 0.0 || totalFrames <= 1)
   {
      outDx = 0.0;
      outDy = 0.0;
      return;
   }
   double frac = static_cast<double>(frameIndex) / static_cast<double>(totalFrames - 1);
   frac = std::min(std::max(frac, 0.0), 1.0);
   double mag = driftPx * frac;
   outDx = mag * std::cos(driftAngleRad_);
   outDy = mag * std::sin(driftAngleRad_);
}

} // namespace sim
