///////////////////////////////////////////////////////////////////////////////
// FILE:          SMLMNoise.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   RNG helpers and the per-pixel camera noise chain used by the
//                synthetic SMLM camera: photon shot noise -> read noise ->
//                gain -> fixed-pattern pixel offset.
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

// Converts a clean photon-count image into a 16-bit ADU frame by applying, in
// order: Poisson shot noise on the photon signal, additive Gaussian read
// noise (in electrons), division by gain (photons/ADU), and the static
// per-pixel fixed-pattern offset. Result is clamped to [0, 65535].
void ApplyNoiseChain(const std::vector<float>& photonImage,
                      std::vector<uint16_t>& outAdu,
                      unsigned width, unsigned height,
                      double gainPhotonsPerAdu,
                      double readNoiseElectrons,
                      const PixelOffsetMap& offsetMap,
                      std::mt19937_64& rng);

} // namespace sim
