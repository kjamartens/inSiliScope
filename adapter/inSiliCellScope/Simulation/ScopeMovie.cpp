///////////////////////////////////////////////////////////////////////////////
// FILE:          ScopeMovie.cpp
// PROJECT:       insilicell
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   See ScopeMovie.h.
//
// LICENSE:       BSD (see license.txt)

#include "ScopeMovie.h"

#include "CellFieldSource.h"
#include "SMLMNoise.h"
#include "SMLMSimulation.h"

#include "insilicell/insilicell.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace sim {

const std::vector<ScopeOption>& ScopeMovieOptions()
{
   static const std::vector<ScopeOption> opts = {
      { "seed", 42, "SimType_RandomSeed (cell field = seed ^ 0x43454C4C unless world-seed >= 0; noise as the adapter)" },
      { "world-seed", -1, "cell-field world seed used as is (the viewer's seed); -1 = derive it from seed" },
      { "x", 0, "FOV centre x, world um (XY stage position)" },
      { "y", 0, "FOV centre y, world um" },
      { "z", 0.5, "Z stage: focal-plane height above the coverslip, um (as the ZStage device; it starts at 0.5)" },
      { "size", 128, "FOV width = height, pixels" },
      { "frames", 1000, "number of frames" },
      { "exposure-ms", 50, "frame duration, ms (simulated time per frame)" },
      { "start-sec", 0, "simulated time of the first frame, s" },
      { "pixel-nm", 100, "pixel size, nm" },
      { "photons-per-sec", 7500, "FluoParam_PhotonsPerSecond" },
      { "on-sec", 0.05, "FluoParam_OnLifetimeSec" },
      { "off-sec", 1.0, "FluoParam_OffLifetimeSec" },
      { "bleach-prob", 1.0, "FluoParam_BlinkBleachProb" },
      { "photon-cv", 0.0, "FluoParam_PhotonCV" },
      { "background-per-sec", 0, "Background_BackgroundPhotonsPerSec (photons/pixel/s)" },
      { "wavelength-nm", 660, "emission wavelength (Gaussian sigma = 0.21 lambda / NA)" },
      { "na", 1.4, "numerical aperture" },
      { "focus-um", 0, "SimType_CellFieldFocusHeightUm (focus offset added to z)" },
      { "z-range-um", 7.0, "SimType_CellFieldZRangeUm: dyes within +/- z-range/2 of the focal plane are rendered (0 = all)" },
      { "activation-rate", 0.01, "SimType_CellFieldActivationRatePerDyePerSec (per dark dye, 1/s)" },
      { "labeling-pct", 10, "SimType_CellFieldLabelingPct (bleaching dyes, % of lattice sites)" },
      { "nonbleach-labeling-pct", 0, "SimType_CellFieldNonBleachingLabelingPct (persistent, DNA-PAINT-like sites)" },
      { "chunk-um", 26, "SimType_CellFieldChunkSizeUm" },
      { "occupancy", 0.33, "SimType_CellFieldOccupancy" },
      { "cell-diam-min-um", 25, "SimType_CellFieldCellDiameterMinUm" },
      { "cell-diam-max-um", 35, "SimType_CellFieldCellDiameterMaxUm" },
      { "mt-density", 0.9, "SimType_CellFieldMicrotubuleDensityPerUm2" },
      { "packing", 1, "SimType_CellFieldPacking (1 on, 0 off)" },
      { "qe", 0.85, "CamParam_QuantumEfficiency" },
      { "dark-per-sec", 1.03, "CamParam_DarkCurrentElectronsPerSec" },
      { "gain", 0.25, "CamParam_GainPhotonsPerADU" },
      { "offset", 100, "CamParam_OffsetADU" },
      { "offset-std", 0.5, "CamParam_OffsetStdADU" },
      { "read-noise", 1.2, "CamParam_ReadNoiseElectrons" },
      { "gain-std-pct", 5, "CamParam_GainStdPctPerPixel" },
      { "read-noise-std-pct", 20, "CamParam_ReadNoiseStdPctPerPixel" },
   };
   return opts;
}

bool ScopeSpecSet(ScopeSpec& spec, const std::string& name, double value)
{
   if (name.compare(0, 2, "p.") == 0 && name.size() > 2)
   {
      spec[name] = value;
      return true;
   }
   for (const ScopeOption& o : ScopeMovieOptions())
      if (name == o.name)
      {
         spec[name] = value;
         return true;
      }
   return false;
}

double ScopeSpecGet(const ScopeSpec& spec, const char* name)
{
   auto it = spec.find(name);
   if (it != spec.end())
      return it->second;
   for (const ScopeOption& o : ScopeMovieOptions())
      if (!std::strcmp(o.name, name))
         return o.value;
   return 0.0;
}

bool ParseScopeSpec(const std::string& text, ScopeSpec& spec, std::string& err)
{
   size_t i = 0;
   while (i < text.size())
   {
      while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == ';' || text[i] == '\n')) i++;
      size_t j = i;
      while (j < text.size() && text[j] != ' ' && text[j] != ',' && text[j] != ';' && text[j] != '\n') j++;
      if (j == i) break;
      const std::string tok = text.substr(i, j - i);
      const size_t eq = tok.find('=');
      char* end = nullptr;
      const double v = eq == std::string::npos ? 0.0 : std::strtod(tok.c_str() + eq + 1, &end);
      if (eq == std::string::npos || end == tok.c_str() + eq + 1 || !ScopeSpecSet(spec, tok.substr(0, eq), v))
      {
         err = "bad option '" + tok + "'";
         return false;
      }
      i = j;
   }
   return true;
}

void ScopeMovieDims(const ScopeSpec& spec, unsigned& w, unsigned& h, long& frames)
{
   w = h = static_cast<unsigned>(std::min(2048.0, std::max(1.0, ScopeSpecGet(spec, "size"))));
   frames = static_cast<long>(std::min(100000.0, std::max(1.0, ScopeSpecGet(spec, "frames"))));
}

bool RenderScopeMovie(const ScopeSpec& spec, const std::function<bool(long, const std::vector<uint16_t>&)>& onFrame,
                      ScopeMovieInfo& info, std::string& err)
{
   auto O = [&](const char* n) { return ScopeSpecGet(spec, n); };
   const auto t0 = std::chrono::steady_clock::now();
   const long seed = static_cast<long>(O("seed"));
   unsigned W, H;
   long N;
   ScopeMovieDims(spec, W, H, N);
   const double expSec = std::max(1e-6, O("exposure-ms") / 1000.0);
   const double t0Sec = std::max(0.0, O("start-sec"));

   // Frame-equivalent parameters, as the camera's SnapshotParams().
   SimulationParams p;
   p.pixelSizeNm = O("pixel-nm");
   p.photonsPerBlink = O("photons-per-sec") * expSec;
   p.backgroundPhotons = O("background-per-sec") * expSec;
   p.psfSigmaPx = std::min(std::max(0.21 * O("wavelength-nm") / std::max(0.01, O("na")) / p.pixelSizeNm, 0.3), 20.0);
   p.quantumEfficiency = O("qe");
   p.darkCurrentElectronsPerFrame = O("dark-per-sec") * expSec;
   p.gainPhotonsPerAdu = O("gain");
   p.offsetAdu = O("offset");
   p.offsetStdAdu = O("offset-std");
   p.readNoiseElectrons = O("read-noise");
   p.pixelGainStdFraction = O("gain-std-pct") / 100.0;
   p.pixelReadNoiseStdFraction = O("read-noise-std-pct") / 100.0;
   p.frameDurationSec = expSec;

   CellFieldSettings cf;
   const double worldSeed = O("world-seed");
   cf.seed = worldSeed >= 0 ? static_cast<uint32_t>(static_cast<uint64_t>(worldSeed))
                            : static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x43454C4CULL);
   std::map<std::string, double> world = {
      { "chunkSize", O("chunk-um") }, { "density", O("occupancy") },
      { "cellDiamMin", O("cell-diam-min-um") }, { "cellDiamMax", O("cell-diam-max-um") },
      { "mtDensity", O("mt-density") }, { "labelEfficiency", O("labeling-pct") / 100.0 },
      { "labelNonBleaching", O("nonbleach-labeling-pct") / 100.0 },
      { "enablePacking", O("packing") != 0 ? 1.0 : 0.0 },
   };
   // p.* pass-through (known core names only), overriding the named ones.
   IscParams* probe = isc_params_new();
   for (const auto& kv : spec)
      if (kv.first.compare(0, 2, "p.") == 0 && isc_params_set(probe, kv.first.c_str() + 2, kv.second) == 0)
         world[kv.first.substr(2)] = kv.second;
   isc_params_free(probe);
   cf.params.assign(world.begin(), world.end());
   cf.activationRatePerSec = O("activation-rate");
   cf.onSec = std::max(1e-6, O("on-sec"));
   cf.offSec = std::max(0.0, O("off-sec"));
   cf.bleachProb = O("bleach-prob");
   cf.photonCV = O("photon-cv");

   // The camera's CellFieldQueryFor (no drift).
   const double um = p.pixelSizeNm / 1000.0, margin = 2.0;
   CellFieldQuery q;
   q.originXUm = O("x") - W * um / 2;
   q.originYUm = O("y") - H * um / 2;
   q.x0Um = q.originXUm - margin;
   q.x1Um = q.originXUm + W * um + margin;
   q.y0Um = q.originYUm - margin;
   q.y1Um = q.originYUm + H * um + margin;
   q.zRefUm = O("focus-um");
   q.zCullCentreUm = O("focus-um") + O("z");
   q.zHalfRangeUm = std::max(0.0, O("z-range-um")) / 2;
   q.frameSec = expSec;
   q.tSec = t0Sec;
   q.spanSec = N * expSec;
   q.frameIndex = 0;

   CellFieldSource source;
   std::vector<BlinkEvent> events;
   if (!source.Configure(cf, err))
      return false;
   if (!source.Events(q, events))
   {
      err = "cell-field event query failed";
      return false;
   }
   info.width = W;
   info.height = H;
   info.frames = N;
   info.blinks = events.size();
   info.querySec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   char desc[512];
   std::snprintf(desc, sizeof desc,
                 "insilicell seed=%ld world_seed=%u x=%g y=%g z=%g size=%u pixel_nm=%g exposure_ms=%g start_sec=%g "
                 "frames=%ld focus_um=%g activation_rate=%g on_sec=%g off_sec=%g bleach_prob=%g photons_per_sec=%g",
                 seed, cf.seed, O("x"), O("y"), O("z"), W, p.pixelSizeNm, expSec * 1000, t0Sec, N, O("focus-um"),
                 cf.activationRatePerSec, cf.onSec, cf.offSec, cf.bleachProb, O("photons-per-sec"));
   info.description = desc;

   // Same streams as the camera's StackGenerationWorker: maps off
   // mt19937_64(seed), counter-based noise on seed ^ 0x9E3779B9.
   std::mt19937_64 rng(static_cast<uint64_t>(seed));
   PixelOffsetMap offsetMap;
   offsetMap.Generate(W, H, p.offsetAdu, p.offsetStdAdu, rng);
   PixelGainMap gainMap;
   gainMap.Generate(W, H, p.gainPhotonsPerAdu, p.pixelGainStdFraction, rng);
   PixelReadNoiseMap rnMap;
   rnMap.Generate(W, H, p.readNoiseElectrons, p.pixelReadNoiseStdFraction, rng);
   const uint32_t noiseSeed = static_cast<uint32_t>(static_cast<uint64_t>(seed) ^ 0x9E3779B9ULL);
   const std::vector<std::vector<uint32_t>> buckets = BucketEventsByFrame(events, N);

   std::vector<float> photons;
   std::vector<uint16_t> adu;
   std::vector<BlinkEvent> fe;
   for (long f = 0; f < N; f++)
   {
      fe.clear();
      for (uint32_t i : buckets[static_cast<size_t>(f)])
         fe.push_back(events[i]);
      RenderPhotonImage(photons, W, H, fe, f, p.pixelSizeNm, p.psfSigmaPx, p.photonsPerBlink, p.backgroundPhotons,
                        0.0, 0.0, nullptr, O("z"));
      ApplyNoiseChain(photons, adu, W, H, p.Camera(), offsetMap, gainMap, rnMap, noiseSeed,
                      static_cast<uint32_t>(f));
      if (!onFrame(f, adu))
         break;
   }
   info.totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   return true;
}

} // namespace sim
