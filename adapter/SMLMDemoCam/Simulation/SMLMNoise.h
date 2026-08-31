///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMNoise.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   RNG helpers and the per-pixel camera noise chain used by the
//                synthetic SMLM camera: quantum efficiency -> dark current ->
//                photon shot noise -> read noise -> gain -> fixed-pattern
//                pixel offset. Read noise and gain each optionally vary
//                per-pixel (sCMOS-style PixelReadNoiseMap/PixelGainMap)
//                instead of being one scalar for the whole sensor.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace sim {

// Standard normal / Poisson draws. Thin wrappers around <random> so callers
// don't need to construct their own distribution objects each call.
double GaussianRng(std::mt19937_64& rng, double mean, double stdDev);
double PoissonRng(std::mt19937_64& rng, double mean);

// Draws Poisson-shot-noised(meanPhotons) + Gaussian(0, readNoiseElectrons) in
// one step. Equivalent to PoissonRng(...) + readNoiseElectrons*GaussianRng(...)
// but roughly twice as fast for meanPhotons large enough that PoissonRng
// itself falls back to a Gaussian approximation: two independent Gaussians
// summed is itself Gaussian with combined variance, so that case only needs
// one GaussianRng call instead of two. Used by ApplyNoiseChain, which calls
// this once per pixel per frame -- hundreds of millions of times for a full
// precomputed stack, where this saved call matters.
double CombinedShotAndReadNoise(std::mt19937_64& rng, double meanPhotons, double readNoiseElectrons);

// A static per-pixel fixed-pattern offset map (offset + offsetStd*Gaussian()
// per pixel), generated once per RandomSeed/size/binning and reused across
// every frame -- matches real sensor fixed-pattern offset noise, which does
// not re-randomize frame to frame.
struct PixelOffsetMap
{
   unsigned width = 0;
   unsigned height = 0;
   std::vector<float> offset; // ADU, size width*height

   void Generate(unsigned w, unsigned h, double offsetMeanAdu, double offsetStdAdu,
                  std::mt19937_64& rng);
};

// A static per-pixel read-noise map (sCMOS-style: every pixel has its own
// amplifier, so read noise genuinely varies pixel to pixel, unlike EMCCD's
// single electron-multiplying register). Each pixel's read noise is
// nominalReadNoiseElectrons*(1 + stdFraction*Gaussian()), clamped >= 0.
// stdFraction = 0 makes every pixel exactly nominalReadNoiseElectrons,
// equivalent to the old scalar-read-noise behavior. Generated once per
// RandomSeed/size and reused across every frame, like PixelOffsetMap.
struct PixelReadNoiseMap
{
   unsigned width = 0;
   unsigned height = 0;
   std::vector<float> readNoiseElectrons; // electrons rms, size width*height

   void Generate(unsigned w, unsigned h, double nominalReadNoiseElectrons, double stdFraction,
                  std::mt19937_64& rng);
};

// A static per-pixel gain map (sCMOS-style pixel-to-pixel conversion-gain
// variation). Each pixel's gain is
// nominalGainPhotonsPerAdu*(1 + stdFraction*Gaussian()), clamped to stay
// positive. stdFraction = 0 makes every pixel exactly
// nominalGainPhotonsPerAdu, equivalent to the old scalar-gain behavior.
// Generated once per RandomSeed/size and reused across every frame, like
// PixelOffsetMap.
struct PixelGainMap
{
   unsigned width = 0;
   unsigned height = 0;
   std::vector<float> gainPhotonsPerAdu; // size width*height

   void Generate(unsigned w, unsigned h, double nominalGainPhotonsPerAdu, double stdFraction,
                  std::mt19937_64& rng);
};

// Converts a clean photon-count image (incident photons, background
// included) into a 16-bit ADU frame by applying, in order:
//   1. Quantum efficiency: incident photons -> mean detected photoelectrons.
//   2. Dark current: a constant electron count added on top (already in
//      electron units, so NOT scaled by quantumEfficiency -- see
//      darkCurrentElectronsPerFrame).
//   3. Poisson shot noise on that electron mean, plus additive Gaussian read
//      noise (in electrons) -- per-pixel from readNoiseMap when it matches
//      width/height, else the scalar readNoiseElectrons for every pixel.
//   4. Division by gain (photons/ADU) -- per-pixel from gainMap when it
//      matches width/height, else the scalar gainPhotonsPerAdu for every
//      pixel.
//   5. The static per-pixel fixed-pattern offset (offsetMap).
// Result is clamped to [0, 65535].
void ApplyNoiseChain(const std::vector<float>& photonImage,
                      std::vector<uint16_t>& outAdu,
                      unsigned width, unsigned height,
                      double quantumEfficiency,
                      double darkCurrentElectronsPerFrame,
                      double gainPhotonsPerAdu,
                      double readNoiseElectrons,
                      const PixelOffsetMap& offsetMap,
                      const PixelGainMap& gainMap,
                      const PixelReadNoiseMap& readNoiseMap,
                      std::mt19937_64& rng);

} // namespace sim
