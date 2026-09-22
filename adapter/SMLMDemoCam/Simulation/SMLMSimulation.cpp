#include "SMLMSimulation.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinLifetimeFrames = 0.01;

// The multi-blink ("rich") kinetics apply when the molecule can blink more
// than once or a blink's brightness varies -- or when a z override is given
// (the out-of-focus population, which webSMLM always runs through its
// spawnMolecules path). Otherwise the original single-blink code runs,
// draw for draw, so a seeded default movie stays byte-identical.
bool UseRichKinetics(const SimulationParams& p, bool haveZOverride)
{
   return p.blinkBleachProb < 1.0 || p.photonCV > 0.0 || haveZOverride;
}

// Log-normal brightness factor with mean 1 and coefficient of variation cv:
// exp(-s^2/2 + s*N(0,1)), s^2 = ln(1 + cv^2). Exactly 1 (no draw) at cv = 0.
double DrawBrightness(std::mt19937_64& rng, double cv)
{
   if (!(cv > 0.0))
      return 1.0;
   const double s2 = std::log(1.0 + cv * cv);
   return std::exp(-s2 / 2.0 + std::sqrt(s2) * GaussianRng(rng, 0.0, 1.0));
}

double ExpDraw(std::mt19937_64& rng, double mean)
{
   std::uniform_real_distribution<double> unif01(0.0, 1.0);
   double u = std::min(unif01(rng), 0.999999);
   return -mean * std::log(1.0 - u);
}
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

   if (!illum && !bgMap && bgScale == 1.0)
   {
      // Original flat-background path, untouched.
      img.assign(n, static_cast<float>(backgroundPhotons));
   }
   else
   {
      img.resize(n);
      for (size_t i = 0; i < n; ++i)
      {
         double bg = bgMap ? (*bgMap)[i] : backgroundPhotons;
         if (illum)
            bg *= (*illum)[i];
         img[i] = static_cast<float>(bg * bgScale);
      }
   }

   bool useVectorial = psfCache && psfCache->valid;
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
      if (useVectorial)
      {
         // Stage offset and this emitter's own depth ADD: the stage moves
         // the focal plane, the structure places the emitter at its own
         // depth, and what the PSF sees is the difference between the two.
         // Looked up per emitter (zNm varies per emitter) rather than once
         // per frame the way a single shared z used to allow.
         bool clamped = false;
         int zIndex = psfCache->NearestZIndex(globalZOffsetUm + e.zNm / 1000.0, &clamped);
         if (outZTotalCount)
            ++*outZTotalCount;
         if (clamped && outZClampedCount)
            ++*outZClampedCount;
         SplatPsfKernel(img, width, height, *psfCache, zIndex, xPx, yPx, photons, psfCache->interpMode);
      }
      else
      {
         RenderGaussianPSF(img, width, height, xPx, yPx, psfSigmaPx, photons);
      }
   }
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
      int zIndex = cache.NearestZIndex(globalZOffsetUm + e.zNm / 1000.0, &clamped);
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
   return GenerateAllEvents(nFrames, widthUm, heightUm, params, rng, 1.0, {});
}

std::vector<BlinkEvent> EmitterModel::GenerateAllEvents(long nFrames, double widthUm, double heightUm,
                                                          const SimulationParams& params,
                                                          std::mt19937_64& rng, double densityScale,
                                                          const std::function<double(std::mt19937_64&)>& zOverride) const
{
   std::vector<BlinkEvent> events;
   if (!pattern_ || nFrames <= 0)
      return events;

   // Always-on patterns (calibration bead fields -- see
   // IPatternGenerator::AlwaysOnSites) bypass the blinking process entirely:
   // one event per site, spanning the whole movie, so every frame renders
   // the identical set of emitters at full brightness. Consumes ZERO rng
   // draws, so switching to such a pattern cannot shift the noise stream.
   // (Never used for the out-of-focus population: a bead field has none.)
   std::vector<EmitterSite> alwaysOn;
   if (!zOverride && pattern_->AlwaysOnSites(widthUm, heightUm, alwaysOn))
   {
      events.reserve(alwaysOn.size());
      for (const EmitterSite& s : alwaysOn)
         events.push_back({s.xUm, s.yUm, s.zNm, -1.0, static_cast<double>(nFrames) + 1.0});
      return events;
   }

   double lifetime = std::max(params.onLifetimeFrames, kMinLifetimeFrames);
   double area = widthUm * heightUm;
   double density = params.emitterDensity * densityScale;
   std::uniform_real_distribution<double> unif01(0.0, 1.0);

   if (UseRichKinetics(params, static_cast<bool>(zOverride)))
   {
      // Port of webSMLM's spawnMolecules(): molecules (not blinks) arrive as
      // a Poisson process; each blinks, then bleaches with probability
      // pBleach or goes dark for Exp(offLife) and blinks again. The arrival
      // rate is divided by the mean blink count so `density` keeps meaning
      // the density of ON emitters per frame. Each molecule samples its site
      // ONCE, so a continuous pattern's molecule keeps its position across
      // blinks too.
      const double pBleach = std::min(1.0, std::max(0.01, params.blinkBleachProb));
      const double offLife = std::max(params.offLifetimeFrames, kMinLifetimeFrames);
      const double meanBlinks = 1.0 / pBleach;
      const double molSpan = meanBlinks * lifetime + (meanBlinks - 1.0) * offLife;
      const double leadIn = std::min(5.0 * molSpan, 20000.0);
      const double rangeLen = static_cast<double>(nFrames) + leadIn;
      const double molRate = density * area / (lifetime * meanBlinks);
      const long nMol = static_cast<long>(PoissonRng(rng, molRate * rangeLen));
      for (long m = 0; m < nMol; ++m)
      {
         EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
         if (zOverride)
            site.zNm = zOverride(rng);
         double t = -leadIn + unif01(rng) * rangeLen;
         for (int guard = 0; guard < 10000 && t < nFrames; ++guard)
         {
            double tEnd = t + ExpDraw(rng, lifetime);
            double bright = DrawBrightness(rng, params.photonCV);
            if (tEnd > 0.0 && t < nFrames)
            {
               BlinkEvent e{site.xUm, site.yUm, site.zNm, t, tEnd};
               e.brightness = bright;
               events.push_back(e);
            }
            if (unif01(rng) < pBleach)
               break;
            t = tEnd + ExpDraw(rng, offLife);
         }
      }
      return events;
   }

   double leadIn = 5.0 * lifetime;
   double arrivalRate = density * area / lifetime;
   double nEventsMean = arrivalRate * (static_cast<double>(nFrames) + leadIn);
   long nEvents = static_cast<long>(PoissonRng(rng, nEventsMean));

   std::uniform_real_distribution<double> tStartDist(-leadIn, static_cast<double>(nFrames));

   events.reserve(static_cast<size_t>(nEvents));
   for (long i = 0; i < nEvents; ++i)
   {
      EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
      double tStart = tStartDist(rng);
      double u = std::min(unif01(rng), 0.999999);
      double tEnd = tStart - lifetime * std::log(1.0 - u);
      events.push_back({site.xUm, site.yUm, site.zNm, tStart, tEnd});
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
   return AdvanceOneFrame(frameIndex, widthUm, heightUm, params, rng, 1.0, {});
}

std::vector<BlinkEvent> EmitterModel::AdvanceOneFrame(long frameIndex, double widthUm, double heightUm,
                                                        const SimulationParams& params, std::mt19937_64& rng,
                                                        double densityScale,
                                                        const std::function<double(std::mt19937_64&)>& zOverride)
{
   // Always-on patterns: same bypass as GenerateAllEvents above, except that
   // a live stream has no known end frame -- so the (fixed) event list is
   // rebuilt each tick to span exactly this frame. liveActive_ stays empty,
   // which is also what makes a switch back to a blinking pattern clean.
   if (pattern_ && !zOverride)
   {
      std::vector<EmitterSite> alwaysOn;
      if (pattern_->AlwaysOnSites(widthUm, heightUm, alwaysOn))
      {
         std::vector<BlinkEvent> events;
         events.reserve(alwaysOn.size());
         for (const EmitterSite& s : alwaysOn)
            events.push_back({s.xUm, s.yUm, s.zNm, static_cast<double>(frameIndex),
                               static_cast<double>(frameIndex) + 1.0});
         return events;
      }
   }

   const double frameEnd = static_cast<double>(frameIndex + 1);
   const bool rich = UseRichKinetics(params, static_cast<bool>(zOverride));
   const double lifetime = std::max(params.onLifetimeFrames, kMinLifetimeFrames);
   std::uniform_real_distribution<double> unif01(0.0, 1.0);

   if (pattern_)
   {
      double area = widthUm * heightUm;
      double density = params.emitterDensity * densityScale;

      if (rich)
      {
         // Multi-blink: molecules arrive at density*area/(lifetime*meanBlinks)
         // per frame (see GenerateAllEvents); each arrival's first blink
         // starts within this frame. Its later blinks are scheduled below as
         // each ON period ends.
         const double pBleach = std::min(1.0, std::max(0.01, params.blinkBleachProb));
         const double molRate = density * area / (lifetime / pBleach);
         long nNew = static_cast<long>(PoissonRng(rng, molRate));
         for (long i = 0; i < nNew; ++i)
         {
            EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
            if (zOverride)
               site.zNm = zOverride(rng);
            double tStart = static_cast<double>(frameIndex) + unif01(rng);
            BlinkEvent e{site.xUm, site.yUm, site.zNm, tStart, tStart + ExpDraw(rng, lifetime)};
            e.brightness = DrawBrightness(rng, params.photonCV);
            e.moleculeLive = true;
            liveActive_.push_back(e);
         }
      }
      else
      {
         double arrivalRatePerFrame = density * area / lifetime;
         long nNew = static_cast<long>(PoissonRng(rng, arrivalRatePerFrame));

         for (long i = 0; i < nNew; ++i)
         {
            EmitterSite site = pattern_->SampleSite(widthUm, heightUm, rng);
            double tStart = static_cast<double>(frameIndex) + unif01(rng);
            double u = std::min(unif01(rng), 0.999999);
            double tEnd = tStart - lifetime * std::log(1.0 - u);
            liveActive_.push_back({site.xUm, site.yUm, site.zNm, tStart, tEnd});
         }
      }
   }

   // Multi-blink continuation: every blink whose ON period ends before the
   // end of this frame decides now whether its molecule bleaches or goes
   // dark for Exp(offLife) and blinks again. Index loop, not iterators: a
   // continuation can itself end within this frame and is then processed in
   // turn further down the same loop. Uses the CURRENT params, so changing
   // the bleach probability/dark lifetime while streaming takes effect on
   // molecules already in flight.
   {
      const double pBleach = std::min(1.0, std::max(0.01, params.blinkBleachProb));
      const double offLife = std::max(params.offLifetimeFrames, kMinLifetimeFrames);
      for (size_t i = 0; i < liveActive_.size(); ++i)
      {
         if (!liveActive_[i].moleculeLive || liveActive_[i].tEnd >= frameEnd)
            continue;
         liveActive_[i].moleculeLive = false;
         if (unif01(rng) < pBleach)
            continue;
         BlinkEvent next = liveActive_[i];
         next.tStart = liveActive_[i].tEnd + ExpDraw(rng, offLife);
         next.tEnd = next.tStart + ExpDraw(rng, lifetime);
         next.brightness = DrawBrightness(rng, params.photonCV);
         next.moleculeLive = true;
         liveActive_.push_back(next);
      }
   }

   // Retire fully-elapsed events (a pending continuation never is: its
   // moleculeLive flag was cleared above once its tEnd fell in this frame).
   liveActive_.erase(
      std::remove_if(liveActive_.begin(), liveActive_.end(),
                      [frameIndex](const BlinkEvent& e) {
                         return !e.moleculeLive && e.tEnd <= static_cast<double>(frameIndex);
                      }),
      liveActive_.end());

   std::vector<BlinkEvent> overlapping;
   overlapping.reserve(liveActive_.size());
   for (const BlinkEvent& e : liveActive_)
   {
      if (e.tStart < frameEnd && e.tEnd > static_cast<double>(frameIndex))
         overlapping.push_back(e);
   }
   return overlapping;
}

std::vector<EmitterSite> EmitterModel::SampleSitesForHaze(double widthUm, double heightUm, size_t maxSamples,
                                                          std::mt19937_64& rng) const
{
   std::vector<EmitterSite> sites;
   if (!pattern_)
      return sites;
   if (pattern_->AlwaysOnSites(widthUm, heightUm, sites))
      return sites;
   sites.reserve(maxSamples);
   for (size_t i = 0; i < maxSamples; ++i)
      sites.push_back(pattern_->SampleSite(widthUm, heightUm, rng));
   return sites;
}

} // namespace sim
