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
                        double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache* psfCache)
{
   img.assign(static_cast<size_t>(width) * height, static_cast<float>(backgroundPhotons));

   bool useVectorial = psfCache && psfCache->valid;
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
      if (useVectorial)
         SplatPsfKernel(img, width, height, *psfCache, psfCache->NearestZIndex(e.zUm), xPx, yPx, photonsPerBlink * ov);
      else
         RenderGaussianPSF(img, width, height, xPx, yPx, psfSigmaPx, photonsPerBlink * ov);
   }
}

void ComputeDriftOffsetPx(double elapsedSec, double driftNmPerSecX, double pixelSizeNm,
                           double& outDx, double& outDy)
{
   double driftXPx = driftNmPerSecX * elapsedSec / pixelSizeNm;
   outDx = driftXPx;
   outDy = 0.5 * driftXPx;
}

void EmitterModel::SetPattern(std::unique_ptr<IPatternGenerator> pattern)
{
   pattern_ = std::move(pattern);
   liveActive_.clear();
}

void EmitterModel::Reseed(uint64_t /*seed*/)
{
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
   std::normal_distribution<double> zDist(0.0, params.psfZSpreadStdNm / 1000.0);

   events.reserve(static_cast<size_t>(nEvents));
   for (long i = 0; i < nEvents; ++i)
   {
      EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
      double tStart = tStartDist(rng);
      double u = std::min(unif01(rng), 0.999999);
      double tEnd = tStart - lifetime * std::log(1.0 - u);
      double zUm = params.psfZSpreadStdNm > 0.0 ? zDist(rng) : 0.0;
      events.push_back({site.xUm, site.yUm, tStart, tEnd, zUm});
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
      std::normal_distribution<double> zDist(0.0, params.psfZSpreadStdNm / 1000.0);

      for (long i = 0; i < nNew; ++i)
      {
         EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
         double tStart = static_cast<double>(frameIndex) + unif01(rng);
         double u = std::min(unif01(rng), 0.999999);
         double tEnd = tStart - lifetime * std::log(1.0 - u);
         double zUm = params.psfZSpreadStdNm > 0.0 ? zDist(rng) : 0.0;
         liveActive_.push_back({site.xUm, site.yUm, tStart, tEnd, zUm});
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

} // namespace sim
