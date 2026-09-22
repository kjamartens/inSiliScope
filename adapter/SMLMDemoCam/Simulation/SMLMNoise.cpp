#include "SMLMNoise.h"
#include "SMLMCounterRng.h"

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Below this mean, PoissonRng draws exactly (Knuth); at or above it, it falls
// back to a Gaussian approximation.
constexpr double kPoissonExactMeanThreshold = 10.0;
}

double GaussianRng(std::mt19937_64& rng, double mean, double stdDev)
{
   // A direct (non-polar) Box-Muller transform: two uniform draws and a
   // handful of transcendental calls, no rejection loop and no distribution
   // object to construct. std::normal_distribution's typical polar-method
   // implementation involves a rejection loop (draws pairs until inside the
   // unit circle) plus per-call construction overhead. (Used for events and
   // the per-pixel maps; the per-frame noise has its own counter-based
   // draws, SMLMCounterRng.h.) The two uniforms are drawn in separate
   // statements so their order is fixed -- two rng calls in one expression
   // would be evaluated in an unspecified order.
   std::uniform_real_distribution<double> unif(0.0, 1.0);
   double u1 = unif(rng);
   if (u1 < 1e-300)
      u1 = 1e-300;
   double u2 = unif(rng);
   double z0 = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
   return mean + stdDev * z0;
}

double PoissonRng(std::mt19937_64& rng, double mean)
{
   if (mean <= 0.0)
      return 0.0;

   // Deliberately not std::poisson_distribution: constructing that
   // distribution object has nontrivial per-call setup cost. Knuth's
   // algorithm (exact, O(mean) uniform draws) handles small means cheaply;
   // a Gaussian approximation -- accurate to
   // good precision once mean is not tiny -- handles the rest in O(1),
   // same fallback the webSMLM reference implementation uses for its own
   // (much higher) large-mean threshold.
   if (mean < kPoissonExactMeanThreshold)
   {
      std::uniform_real_distribution<double> unif(0.0, 1.0);
      double L = std::exp(-mean);
      double p = 1.0;
      long k = 0;
      do
      {
         ++k;
         p *= unif(rng);
      } while (p > L);
      return static_cast<double>(k - 1);
   }

   double v = mean + std::sqrt(mean) * GaussianRng(rng, 0.0, 1.0);
   return v < 0.0 ? 0.0 : v;
}

void PixelOffsetMap::Generate(unsigned w, unsigned h, double offsetMeanAdu, double offsetStdAdu,
                               std::mt19937_64& rng)
{
   width = w;
   height = h;
   offset.assign(static_cast<size_t>(w) * h, 0.0f);
   for (size_t i = 0; i < offset.size(); ++i)
   {
      offset[i] = static_cast<float>(offsetMeanAdu + offsetStdAdu * GaussianRng(rng, 0.0, 1.0));
   }
}

void PixelReadNoiseMap::Generate(unsigned w, unsigned h, double nominalReadNoiseElectrons,
                                  double stdFraction, std::mt19937_64& rng)
{
   width = w;
   height = h;
   readNoiseElectrons.assign(static_cast<size_t>(w) * h, 0.0f);
   for (size_t i = 0; i < readNoiseElectrons.size(); ++i)
   {
      double v = nominalReadNoiseElectrons * (1.0 + stdFraction * GaussianRng(rng, 0.0, 1.0));
      readNoiseElectrons[i] = static_cast<float>(v < 0.0 ? 0.0 : v);
   }
}

void PixelGainMap::Generate(unsigned w, unsigned h, double nominalGainPhotonsPerAdu, double stdFraction,
                             std::mt19937_64& rng)
{
   width = w;
   height = h;
   gainPhotonsPerAdu.assign(static_cast<size_t>(w) * h, 0.0f);
   for (size_t i = 0; i < gainPhotonsPerAdu.size(); ++i)
   {
      double v = nominalGainPhotonsPerAdu * (1.0 + stdFraction * GaussianRng(rng, 0.0, 1.0));
      // A near-zero or negative pixel gain would blow up the division below;
      // floor it well away from zero rather than letting one unlucky draw
      // (possible at large stdFraction) produce a saturated/garbage pixel.
      gainPhotonsPerAdu[i] = static_cast<float>(v < 0.01 ? 0.01 : v);
   }
}

void ApplyNoiseChain(const std::vector<float>& photonImage,
                      std::vector<uint16_t>& outAdu,
                      unsigned width, unsigned height,
                      const CameraNoiseParams& cam,
                      const PixelOffsetMap& offsetMap,
                      const PixelGainMap& gainMap,
                      const PixelReadNoiseMap& readNoiseMap,
                      uint32_t noiseSeed, uint32_t frame)
{
   const size_t n = static_cast<size_t>(width) * height;
   outAdu.resize(n);
   const bool haveOffsetMap = (offsetMap.width == width && offsetMap.height == height &&
                                offsetMap.offset.size() == n);
   const bool haveGainMap = (gainMap.width == width && gainMap.height == height &&
                              gainMap.gainPhotonsPerAdu.size() == n);
   const bool haveReadNoiseMap = (readNoiseMap.width == width && readNoiseMap.height == height &&
                                   readNoiseMap.readNoiseElectrons.size() == n);

   CounterRng u(noiseSeed, frame);
   if (cam.emccd)
   {
      const double maxAdu = std::ldexp(1.0, std::min(16, std::max(1, cam.bitDepth))) - 1.0;
      const double emGain = std::max(1.0, cam.emGain);
      for (size_t i = 0; i < n; ++i)
      {
         u.Pixel(static_cast<uint32_t>(i));
         const double photons = std::max(0.0, static_cast<double>(photonImage[i]));
         // Integer electron count entering the gain register, which is then
         // Gamma(shape = electrons, scale = 1): variance 2x the mean, the
         // sqrt(2) excess noise factor. Read noise is divided by the EM gain.
         const double ne = CounterPoisson(photons * cam.quantumEfficiency + cam.darkCurrentElectrons + cam.cicElectrons, u);
         const double out = ne > 0.0 ? CounterGamma(ne, u) : 0.0;
         const double readE = (haveReadNoiseMap ? readNoiseMap.readNoiseElectrons[i] : cam.readNoiseElectrons) / emGain;
         const double gain = haveGainMap ? gainMap.gainPhotonsPerAdu[i] : cam.gainPhotonsPerAdu;
         const double offset = haveOffsetMap ? offsetMap.offset[i] : 0.0;
         const double g = CounterGauss(u);
         double adu = std::floor(offset + (out + readE * g) / gain + 0.5);
         outAdu[i] = static_cast<uint16_t>(adu < 0.0 ? 0.0 : (adu > maxAdu ? maxAdu : adu));
      }
      return;
   }

   for (size_t i = 0; i < n; ++i)
   {
      u.Pixel(static_cast<uint32_t>(i));
      const double photons = std::max(0.0, static_cast<double>(photonImage[i]));
      // Quantum efficiency converts incident photons to mean detected
      // photoelectrons; dark current is already in electron units (it
      // originates in the sensor, not in incident light) so it's added
      // after QE, not scaled by it.
      const double ne = CounterPoisson(photons * cam.quantumEfficiency + cam.darkCurrentElectrons, u);
      const double readNoise = haveReadNoiseMap ? readNoiseMap.readNoiseElectrons[i] : cam.readNoiseElectrons;
      const double gain = haveGainMap ? gainMap.gainPhotonsPerAdu[i] : cam.gainPhotonsPerAdu;
      const double offset = haveOffsetMap ? offsetMap.offset[i] : 0.0;
      const double g = CounterGauss(u);
      double adu = offset + (ne + readNoise * g) / gain;
      adu = adu < 0.0 ? 0.0 : (adu > 65535.0 ? 65535.0 : adu);
      outAdu[i] = static_cast<uint16_t>(std::floor(adu + 0.5));
   }
}

} // namespace sim
