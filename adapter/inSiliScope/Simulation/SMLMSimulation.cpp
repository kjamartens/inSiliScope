#include "SMLMSimulation.h"
#include "Parallel.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

namespace {

// RenderGaussianPSF for the image rows [rowLo, rowHi) only.
void RenderGaussianRows(std::vector<float>& img, unsigned width, unsigned height, int rowLo, int rowHi,
                        double xPx, double yPx, double sigmaPx, double totalPhotons)
{
   if (totalPhotons <= 0.0 || sigmaPx <= 0.0)
      return;

   int rad = static_cast<int>(std::ceil(3.0 * sigmaPx));
   int cx = static_cast<int>(std::lround(xPx));
   int cy = static_cast<int>(std::lround(yPx));
   double amplitude = totalPhotons / (2.0 * kPi * sigmaPx * sigmaPx);
   double twoSigmaSq = 2.0 * sigmaPx * sigmaPx;

   int yLo = std::max(std::max(0, rowLo), cy - rad);
   int yHi = std::min(std::min(static_cast<int>(height), rowHi) - 1, cy + rad);
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

// One emitter of a frame, as RenderPhotonImage places it.
struct FrameEmitter
{
   double xPx, yPx, photons;
   int zIndex;
};

} // namespace

void RenderGaussianPSF(std::vector<float>& img, unsigned width, unsigned height,
                        double xPx, double yPx, double sigmaPx, double totalPhotons)
{
   RenderGaussianRows(img, width, height, 0, static_cast<int>(height), xPx, yPx, sigmaPx, totalPhotons);
}

void RenderPhotonImage(std::vector<float>& img, unsigned width, unsigned height,
                        const std::vector<BlinkEvent>& events, long frameIndex,
                        double pixelSizeNm, double psfSigmaPx, double photonsPerBlink,
                        double backgroundPhotons,
                        double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache* psfCache, double globalZOffsetUm,
                        long* outZClampedCount, long* outZTotalCount,
                        const RenderExtras* extras)
{
   const size_t n = static_cast<size_t>(width) * height;
   const std::vector<float>* illum =
      (extras && extras->illumField && extras->illumField->size() == n) ? extras->illumField : nullptr;
   const std::vector<float>* bgMap =
      (extras && extras->backgroundMap && extras->backgroundMap->size() == n) ? extras->backgroundMap : nullptr;
   const double bgScale = extras ? extras->backgroundScale : 1.0;
   const bool flatBackground = !illum && !bgMap && bgScale == 1.0;
   const bool parallel = extras && extras->parallel;
   const bool accumulate = extras && extras->accumulate;

   // Background of the rows [y0, y1).
   auto background = [&](unsigned y0, unsigned y1) {
      if (accumulate)
         return;
      const size_t i0 = static_cast<size_t>(y0) * width, i1 = static_cast<size_t>(y1) * width;
      if (flatBackground)
      {
         // Original flat-background path.
         std::fill(img.begin() + i0, img.begin() + i1, static_cast<float>(backgroundPhotons));
         return;
      }
      for (size_t i = i0; i < i1; ++i)
      {
         double bg = bgMap ? (*bgMap)[i] : backgroundPhotons;
         if (illum)
            bg *= (*illum)[i];
         img[i] = static_cast<float>(bg * bgScale);
      }
   };

   // Every emitter's position, photons and plane, in event order.
   bool useKernel = psfCache && psfCache->valid;
   std::vector<FrameEmitter> ems;
   ems.reserve(events.size());
   for (const BlinkEvent& e : events)
   {
      double ov = std::min(static_cast<double>(frameIndex + 1), e.tEnd) -
                  std::max(static_cast<double>(frameIndex), e.tStart);
      if (ov <= 0.0)
         continue;
      if (ov > 1.0)
         ov = 1.0;

      double sitePxX = e.xUm * 1000.0 / pixelSizeNm;
      double sitePxY = e.yUm * 1000.0 / pixelSizeNm;
      double xPx = sitePxX + driftOffsetXPx;
      double yPx = sitePxY + driftOffsetYPx;
      double photons = photonsPerBlink * ov;
      if (e.brightness != 1.0)
         photons *= e.brightness;
      // Illumination is read at the emitter's own (undrifted) site, as
      // webSMLM's illumAt does -- the beam is fixed to the sample, the
      // drift moves both.
      if (illum)
         photons *= IlluminationAt(*illum, width, height, sitePxX, sitePxY);
      int zIndex = 0;
      if (useKernel)
      {
         // The Z stage position is the focal plane's height and the emitter
         // sits at its own depth: what the PSF sees is the difference, the
         // emitter's height above the focal plane (+Z moves focus up).
         // Looked up per emitter (zNm varies per emitter) rather than once
         // per frame the way a single shared z used to allow.
         bool clamped = false;
         zIndex = psfCache->NearestZIndex(e.zNm / 1000.0 - globalZOffsetUm, &clamped);
         if (outZTotalCount)
            ++*outZTotalCount;
         if (clamped && outZClampedCount)
            ++*outZClampedCount;
      }
      ems.push_back({xPx, yPx, photons, zIndex});
   }

   if (!accumulate)
      img.resize(n);
   if (!parallel)
   {
      background(0, height);
      SplatPlan plan;
      for (const FrameEmitter& em : ems)
      {
         if (!useKernel)
            RenderGaussianRows(img, width, height, 0, static_cast<int>(height), em.xPx, em.yPx, psfSigmaPx,
                               em.photons);
         else if (PlanSplat(*psfCache, em.zIndex, em.xPx, em.yPx, em.photons, psfCache->interpMode, plan))
            SplatRows(img, width, height, 0, static_cast<int>(height), *psfCache, plan, em.photons);
      }
      return;
   }

   // Parallel: bands of rows, each adding every emitter's share in event
   // order, so every pixel sums the same terms in the same order as the
   // serial loop. Fft plans (a transform per emitter) are made once, their
   // line transforms spread over the cores, before the bands.
   std::vector<SplatPlan> plans;
   std::vector<char> planned;
   if (useKernel)
   {
      plans.resize(ems.size());
      planned.assign(ems.size(), 0);
      const bool fft = psfCache->interpMode == PsfInterpMode::Fft;
      for (size_t k = 0; k < ems.size(); ++k)
         planned[k] = PlanSplat(*psfCache, ems[k].zIndex, ems[k].xPx, ems[k].yPx, ems[k].photons,
                                psfCache->interpMode, plans[k], fft);
   }
   const unsigned bands = std::max(1u, std::min(height, 64u));
   ParallelFor(bands, [&](unsigned b) {
      const unsigned y0 = static_cast<unsigned>(static_cast<size_t>(height) * b / bands);
      const unsigned y1 = static_cast<unsigned>(static_cast<size_t>(height) * (b + 1) / bands);
      background(y0, y1);
      for (size_t k = 0; k < ems.size(); ++k)
      {
         if (!useKernel)
            RenderGaussianRows(img, width, height, static_cast<int>(y0), static_cast<int>(y1), ems[k].xPx,
                               ems[k].yPx, psfSigmaPx, ems[k].photons);
         else if (planned[k])
            SplatRows(img, width, height, static_cast<int>(y0), static_cast<int>(y1), *psfCache, plans[k],
                      ems[k].photons);
      }
   });
}

std::vector<std::vector<uint32_t>> BucketEventsByFrame(const std::vector<BlinkEvent>& events, long nFrames)
{
   std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(std::max(0L, nFrames)));
   for (size_t i = 0; i < events.size(); ++i)
   {
      const BlinkEvent& e = events[i];
      if (!(e.tEnd > 0.0) || !(e.tStart < static_cast<double>(nFrames)))
         continue;
      long f0 = std::max(0L, static_cast<long>(std::floor(e.tStart)));
      long f1 = std::min(nFrames - 1, static_cast<long>(std::floor(e.tEnd)));
      for (long f = f0; f <= f1; ++f)
         buckets[static_cast<size_t>(f)].push_back(static_cast<uint32_t>(i));
   }
   return buckets;
}

void CollectGpuEmitters(const std::vector<BlinkEvent>& events, long frameIndex, unsigned width, unsigned height,
                        double pixelSizeNm, double photonsPerBlink, double driftOffsetXPx, double driftOffsetYPx,
                        const PsfKernelCache& cache, double globalZOffsetUm, const RenderExtras* extras,
                        std::vector<GpuSplatEmitter>& out, long* outZClampedCount, long* outZTotalCount)
{
   const size_t n = static_cast<size_t>(width) * height;
   const std::vector<float>* illum =
      (extras && extras->illumField && extras->illumField->size() == n) ? extras->illumField : nullptr;
   for (const BlinkEvent& e : events)
   {
      // Mirrors RenderPhotonImage's per-emitter arithmetic exactly.
      double ov = std::min(static_cast<double>(frameIndex + 1), e.tEnd) -
                  std::max(static_cast<double>(frameIndex), e.tStart);
      if (ov <= 0.0)
         continue;
      if (ov > 1.0)
         ov = 1.0;
      double sitePxX = e.xUm * 1000.0 / pixelSizeNm;
      double sitePxY = e.yUm * 1000.0 / pixelSizeNm;
      double photons = photonsPerBlink * ov;
      if (e.brightness != 1.0)
         photons *= e.brightness;
      if (illum)
         photons *= IlluminationAt(*illum, width, height, sitePxX, sitePxY);
      bool clamped = false;
      int zIndex = cache.NearestZIndex(e.zNm / 1000.0 - globalZOffsetUm, &clamped);
      if (outZTotalCount)
         ++*outZTotalCount;
      if (clamped && outZClampedCount)
         ++*outZClampedCount;
      if (photons <= 0.0)
         continue;
      SplatSetupResult st = SplatSetup(cache, sitePxX + driftOffsetXPx, sitePxY + driftOffsetYPx, cache.interpMode);
      GpuSplatEmitter g = {};
      g.x0 = st.x0;
      g.y0 = st.y0;
      g.bx = st.bx;
      g.by = st.by;
      g.plane = zIndex;
      g.nTaps = st.nTaps;
      g.photons = static_cast<float>(photons);
      for (int k = 0; k < 4; ++k)
      {
         g.wx[k] = static_cast<float>(st.wx[k]);
         g.wy[k] = static_cast<float>(st.wy[k]);
      }
      out.push_back(g);
   }
}

void ComputeDriftOffsetPx(double elapsedSec, double driftNmPerSec, double angleRad, double pixelSizeNm,
                           double& outDx, double& outDy)
{
   double driftPx = driftNmPerSec * elapsedSec / pixelSizeNm;
   outDx = driftPx * std::cos(angleRad);
   outDy = driftPx * std::sin(angleRad);
}

double DriftAngleForSeed(long seed)
{
   std::mt19937_64 rng(static_cast<uint64_t>(seed) ^ 0x4452494654444952ULL); // "DRIFTDIR"
   std::uniform_real_distribution<double> unif01(0.0, 1.0);
   return unif01(rng) * 2.0 * kPi;
}

} // namespace sim
