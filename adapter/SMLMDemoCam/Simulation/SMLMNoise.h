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
//                instead of being one scalar for the whole sensor. An
//                optional EMCCD path (CameraNoiseParams::emccd) replaces
//                shot+read noise with an electron-multiplying register --
//                ported from webSMLM's applySimCameraNoise.
//
// LICENSE:       BSD (see license.txt)

#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace sim {

// Standard normal / Poisson draws on a sequential stream -- used for the
// emitter events and the static per-pixel maps. (The per-frame camera noise
// uses the counter-based draws in SMLMCounterRng.h instead.)
double GaussianRng(std::mt19937_64& rng, double mean, double stdDev);
double PoissonRng(std::mt19937_64& rng, double mean);

// Everything ApplyNoiseChain needs beyond the per-pixel maps: the scalar
// sensor model. emccd selects the electron-multiplying path (see
// ApplyNoiseChain); emGain/cicElectrons/bitDepth are used only there.
struct CameraNoiseParams
{
   double quantumEfficiency = 0.85;       // incident photons -> detected electrons
   double darkCurrentElectrons = 0.0;     // thermal dark counts this frame
   double gainPhotonsPerAdu = 1.0;
   double readNoiseElectrons = 1.5;
   bool emccd = false;
   // EM gain. Only divides the read noise (readNoise/max(1,emGain)): the
   // register is modelled as Gamma(shape=electrons, scale=1), so gainPhotons-
   // PerAdu keeps meaning photons/ADU on both paths and the excess noise is
   // the register's own statistics (variance 2x the mean -- the sqrt(2)
   // excess noise factor), exactly as webSMLM does it.
   double emGain = 300.0;
   double cicElectrons = 0.002;           // clock-induced charge, e-/pixel/frame
   int bitDepth = 16;                     // EMCCD ADU are clipped to [0, 2^bitDepth-1]
};

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
// included) into a 16-bit ADU frame, drawing every random number from the
// counter-based stream (noiseSeed, frame, pixel) -- SMLMCounterRng.h -- so a
// frame's noise does not depend on which thread or device made it, or on
// any other frame (webSMLM build 2026-09-21b). The GPU kernel
// (GpuSimD3D11.cpp) applies the identical chain. sCMOS path (cam.emccd ==
// false), in order:
//   1. Quantum efficiency: incident photons -> mean detected photoelectrons.
//   2. Dark current: a constant electron count added on top (already in
//      electron units, so NOT scaled by quantumEfficiency -- see
//      darkCurrentElectronsPerFrame).
//   3. Poisson shot noise on that electron mean (webSMLM's simNoisePoisson:
//      exact below mean 60, a rounded normal approximation above), plus
//      additive Gaussian read noise (in electrons) -- per-pixel from
//      readNoiseMap when it matches width/height, else the scalar
//      readNoiseElectrons for every pixel. Poisson draws first, then the
//      read-noise normal, always in that order.
//   4. Division by gain (photons/ADU) -- per-pixel from gainMap when it
//      matches width/height, else the scalar gainPhotonsPerAdu for every
//      pixel.
//   5. The static per-pixel fixed-pattern offset (offsetMap).
// Result is clamped to [0, 65535] and rounded.
//
// EMCCD path (cam.emccd == true, port of webSMLM's applySimCameraNoise
// 'emccd' branch): ne = Poisson(QE*photons + dark + CIC) (an integer
// electron count), then the gain register out = Gamma(ne, 1) (0 if ne = 0),
// then + Gaussian read noise of readNoise/max(1,emGain) electrons, / gain,
// + offset, rounded and clipped to [0, 2^bitDepth - 1]. The per-pixel gain
// and read-noise maps still apply. Unlike webSMLM (which has no dark
// current), dark current is added to the Poisson mean alongside CIC.
void ApplyNoiseChain(const std::vector<float>& photonImage,
                      std::vector<uint16_t>& outAdu,
                      unsigned width, unsigned height,
                      const CameraNoiseParams& cam,
                      const PixelOffsetMap& offsetMap,
                      const PixelGainMap& gainMap,
                      const PixelReadNoiseMap& readNoiseMap,
                      uint32_t noiseSeed, uint32_t frame);

} // namespace sim
